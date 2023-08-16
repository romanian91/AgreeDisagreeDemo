/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2015 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*******************************************************************************/

#include "../Bricks/port.h"

#include <queue>

#include "schema.h"
#include "db.h"
#include "dashboard.h"

CEREAL_REGISTER_TYPE_WITH_NAME(schema::Record, "0");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::UserRecord, "U");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::QuestionRecord, "Q");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::AnswerRecord, "A");

#include "../Bricks/file/file.h"
#include "../Bricks/strings/util.h"
#include "../Bricks/time/chrono.h"
#include "../Bricks/rtti/dispatcher.h"
#include "../Bricks/net/api/api.h"
#include "../Bricks/mq/inmemory/mq.h"
#include "../Bricks/graph/gnuplot.h"
#include "../Bricks/waitable_atomic/waitable_atomic.h"
#include "../Bricks/dflags/dflags.h"
#include "../Bricks/util/singleton.h"
#include "../fncas/fncas/fncas.h"

// TODO(dkorolev): Move this into Bricks.
#include "bricks-cerealize-multikeyjson.h"

DEFINE_int32(port, 3000, "Local port to use.");

using bricks::FileSystem;
using bricks::strings::Printf;
using bricks::WaitableAtomic;
using bricks::time::Now;
using bricks::time::EPOCH_MILLISECONDS;
using bricks::time::MILLISECONDS_INTERVAL;

template <typename Y>
struct VizPoint {
  double x;
  Y y;
  template <typename A>
  void serialize(A& ar) {
    ar(CEREAL_NVP(x), CEREAL_NVP(y));
  }
  EPOCH_MILLISECONDS ExtractTimestamp() const { return static_cast<EPOCH_MILLISECONDS>(x); }
};

// The current state of an instance of the demo.
struct Snapshot {
  // The `Box` structure encapsulates the state of the demo.
  // All calls to it, updates and reads, go through the message queue, and thus are sequential.
  struct Box {
    std::vector<std::string> users;
    std::vector<std::string> questions;
    std::map<schema::QID, std::map<schema::UID, schema::ANSWER>> answers;
  };
  // The `SlidingWindowTracker` structure keeps track of engagement-related events at real time.
  struct SlidingWindowTracker {
    const double w_;
    mutable std::queue<double> q_;
    explicit SlidingWindowTracker(const double w = 15000.0) : w_(w) {}
    void AddAction(double t) {
      q_.push(t);
      Relax(t);
    }
    int GetValueOverSlidingWindow(double t) const {
      Relax(t);
      return static_cast<int>(q_.size());
    }
    void Relax(double t) const {
      while (!q_.empty() && (t - q_.front()) > w_ + 1e-9) {
        q_.pop();
      }
    }
  };

  // Data fields.
  Box box;
  SlidingWindowTracker engagement;
};

// The `Cruncher` defines a real (no shit!) TailProduce worker.
// It maintains the consistency of the `Snapshot` and allows access to it.
//
// `Cruncher` works with two inputs of two "universes":
// 1) The `storage::Record` entries, which update the state of the `Snapshot`, and
// 2) The `*MQMessage` messages, which use the `Snapshot` for API responses or for other needs (ex. timer).
//
// The inputs of both universes get delivered to the `Cruncher` via the message queue.
// Thus, they are processed sequentially, and no multithreading collisions can occur in the meantime.
class Cruncher final {
 public:
  Cruncher(int port, const std::string& demo_id)
      : demo_id_(demo_id),
        u_total_(sherlock::Stream<VizPoint<int>>(demo_id_ + "_u_total", "point")),
        q_total_(sherlock::Stream<VizPoint<int>>(demo_id_ + "_q_total", "point")),
        e_15sec_(sherlock::Stream<VizPoint<int>>(demo_id_ + "_e_15sec", "point")),
        image_(sherlock::Stream<VizPoint<std::string>>(demo_id_ + "_image", "point")),
        consumer_(demo_id_, image_),
        mq_(consumer_),
        metronome_thread_(&Cruncher::MetronomeThread, this) {
    try {
      // Data streams.
      HTTP(port).Register("/" + demo_id_ + "/layout/d/u", u_total_);
      HTTP(port).Register("/" + demo_id_ + "/layout/d/q", q_total_);
      HTTP(port).Register("/" + demo_id_ + "/layout/d/e", e_15sec_);
      HTTP(port).Register("/" + demo_id_ + "/layout/d/i", image_);

      // The black magic of serving the dashboard.
      HTTP(port).ServeStaticFilesFrom(FileSystem::JoinPath("static", "js"), "/" + demo_id_ + "/static/");

      HTTP(port).Register("/" + demo_id_ + "/config", [this](Request r) {
        // Read the file once.
        static const std::string dashboard_template =
            bricks::FileSystem::ReadFileAsString(bricks::FileSystem::JoinPath("static", "template.html"));
        // Build the placeholder replacements.
        std::map<std::string, std::string> replacement_map = {
            // Custom style tags in the `<head>`, if needed.
            {"<style id=\"knsh-dashboard-style-placeholder\"></style>", ""},
            // Header columns between the logo and the GitHub link.
            {"<div class=\"knsh-columns__item\" id=\"knsh-header-columns-placeholder\"></div>",
             "<div class=\"knsh-columns__item\" style=\"text-align: right;\">"
             "<a href=\"/" +
                 demo_id_ +
                 "/a/\" class=\"knsh-header-link\"><span>Actions</span></a>"
                 "</div>"},
            // Footer columns between the copyright and the GitHub link.
            {"<div class=\"knsh-columns__item\" id=\"knsh-footer-columns-placeholder\"></div>", ""},
            // Anything to put above the generated dashboard.
            {"<div id=\"knsh-dashboard-before-placeholder\"></div>", ""},
            // Anything to put below the generated dashboard.
            {"<div id=\"knsh-dashboard-after-placeholder\"></div>", ""}};
        // Replace the placeholders with the replacements.
        std::string dashboard_template_output = dashboard_template;
        for (const auto& kv : replacement_map) {
          std::size_t pos = 0;
          while (std::string::npos != (pos = dashboard_template_output.find(kv.first, pos))) {
            dashboard_template_output.replace(pos, kv.first.length(), kv.second);
            pos += kv.second.length();
          }
        }
        // The layout URL is an absolute URL, not relative to the config URL.
        r(dashboard::Config("/" + demo_id_ + "/layout", dashboard_template_output), "config");
      });

      HTTP(port).Register("/" + demo_id_ + "/layout", [](Request r) {
        using namespace dashboard::layout;
        r(Layout(Row({Col({Cell("/q_meta"), Cell("/u_meta"), Cell("/e_meta")}), Cell("/i_meta")})), "layout");
      });

      HTTP(port).Register("/" + demo_id_ + "/layout/u_meta", [](Request r) {
        auto meta = dashboard::PlotMeta();
        meta.options.caption = "Total users.";
        meta.data_url = "/d/u";
        r(meta, "meta");
      });

      HTTP(port).Register("/" + demo_id_ + "/layout/q_meta", [](Request r) {
        auto meta = dashboard::PlotMeta();
        meta.options.caption = "Total questions.";
        meta.data_url = "/d/q";
        r(meta, "meta");
      });

      HTTP(port).Register("/" + demo_id_ + "/layout/e_meta", [](Request r) {
        auto meta = dashboard::PlotMeta();
        meta.options.caption = "15-Seconds Engagement.";
        meta.data_url = "/d/e";
        r(meta, "meta");
      });

      HTTP(port).Register("/" + demo_id_ + "/layout/i_meta", [](Request r) {
        auto meta = dashboard::ImageMeta();
        meta.options.header_text = "Agreement between users.";
        meta.data_url = "/d/i";
        r(meta, "meta");
      });

      // Need a dedicated handler for '$DEMO_ID/' to serve the nicely looking dashboard.
      HTTP(port).Register(
          "/" + demo_id_ + "/",
          new bricks::net::api::StaticFileServer(
              bricks::FileSystem::ReadFileAsString(bricks::FileSystem::JoinPath("static", "index.html")),
              "text/html"));

      HTTP(port).Register("/" + demo_id_ + "/layout/d/i/viz.png",
                          [this](Request r) { mq_.EmplaceMessage(new VizMQMessage(std::move(r))); });
    } catch (const bricks::Exception& e) {
      std::cerr << "Crunched constructor exception: " << e.What() << std::endl;
      throw;
    }
  }

  ~Cruncher() {
    // TODO(dkorolev): There should probably be a better, more Bricks-standard way to make use of a metronome.
    metronome_thread_.join();
  }

  struct FunctionMQMessage : schema::Base {
    std::function<void(Snapshot&)> function_with_snapshot;
    FunctionMQMessage() = delete;
    explicit FunctionMQMessage(std::function<void(Snapshot&)> f) : function_with_snapshot(f) {}
  };

  struct HTTPRequestMQMessage : schema::Base {
    Request request;
    std::function<void(Request, Snapshot&)> http_function_with_snapshot;
    HTTPRequestMQMessage() = delete;
    explicit HTTPRequestMQMessage(Request r, std::function<void(Request, Snapshot&)> f)
        : request(std::move(r)), http_function_with_snapshot(f) {}
  };

  struct VizMQMessage : schema::Base {
    Request request;
    VizMQMessage() = delete;
    explicit VizMQMessage(Request r) : request(std::move(r)) {}
  };

  struct TickMQMessage : schema::Base {
    typedef sherlock::StreamInstance<VizPoint<int>> stream_type;
    stream_type& p_u_total;
    stream_type& p_q_total;
    stream_type& p_e_15sec;
    TickMQMessage() = delete;
    TickMQMessage(stream_type& u, stream_type& p, stream_type& e) : p_u_total(u), p_q_total(p), p_e_15sec(e) {}
  };

  inline bool Entry(std::unique_ptr<schema::Base>& entry, size_t index, size_t total) {
    static_cast<void>(index);
    static_cast<void>(total);
    // Note: The following call transfers ownership away from the passed in `unique_ptr`
    // into the `unique_ptr` in the message queue.
    // Looks straighforward to me after refactoring everything around it, yet comments and very welcome. -- D.K.
    mq_.EmplaceMessage(entry.release());
    return true;
  }

  inline void Terminate() { std::cerr << '@' << demo_id_ << " is done.\n"; }

  void CallFunctionWithSnapshot(std::function<void(Snapshot&)> f) {
    mq_.EmplaceMessage(new FunctionMQMessage(f));
  }

  void ServeRequestWithSnapshot(Request r, std::function<void(Request, Snapshot&)> f) {
    mq_.EmplaceMessage(new HTTPRequestMQMessage(std::move(r), f));
  }

  struct Consumer {
    const std::string& demo_id_;
    Snapshot snapshot_;

    // Syncronization between the consumer thread that the thread that updates models and images
    // is done via a lockable and waitable object.
    struct Visualization {
      // Increment this index to initiate model and image refresh.
      size_t requested = 0;
      // This index is either equal to `requested` ("caught up") or is less than it ("behind").
      size_t done = 0;
      // Copy of the data to generate the image for.
      Snapshot::Box box;
      // The image that is currently on display.
      std::string image = "";
    };
    WaitableAtomic<Visualization> visualization_;

    sherlock::StreamInstance<VizPoint<std::string>>& image_stream_;

    std::thread visualization_thread_;

    Consumer() = delete;
    Consumer(const std::string& demo_id, sherlock::StreamInstance<VizPoint<std::string>>& image_stream)
        : demo_id_(demo_id),
          image_stream_(image_stream),
          visualization_thread_(&Consumer::UpdateVisualizationThread, this) {}

    inline void OnMessage(std::unique_ptr<schema::Base>& message, size_t) {
      struct types {
        typedef schema::Base base;
        typedef std::tuple<schema::AnswerRecord,
                           schema::QuestionRecord,
                           schema::UserRecord,
                           FunctionMQMessage,
                           HTTPRequestMQMessage,
                           VizMQMessage,
                           TickMQMessage> derived_list;
        typedef bricks::rtti::RuntimeTupleDispatcher<base, derived_list> dispatcher;
      };
      types::dispatcher::DispatchCall(*message, *this);
    }

    inline void operator()(schema::Base&) { throw std::logic_error("Should not happen (schema::Base)."); }
    inline void operator()(schema::Record&) { throw std::logic_error("Should not happen (schema::Record)."); }

    inline void operator()(schema::UserRecord& u) {
      std::cerr << '@' << demo_id_ << " +U: " << u.uid << '\n';
      snapshot_.box.users.push_back(u.uid);
      snapshot_.engagement.AddAction(static_cast<double>(u.ms));
      TriggerVisualizationUpdate();
    }

    inline void operator()(schema::QuestionRecord& q) {
      std::cerr << '@' << demo_id_ << " +Q" << static_cast<size_t>(q.qid) << " : \"" << q.text << "\"\n";
      snapshot_.box.questions.push_back(q.text);
      snapshot_.engagement.AddAction(static_cast<double>(q.ms));
    }

    inline void operator()(schema::AnswerRecord& a) {
      std::cerr << '@' << demo_id_ << " +A: " << a.uid << " `" << static_cast<int>(a.answer) << "` Q"
                << static_cast<size_t>(a.qid) << '\n';
      snapshot_.box.answers[a.qid][a.uid] = a.answer;
      snapshot_.engagement.AddAction(static_cast<double>(a.ms));
      TriggerVisualizationUpdate();
    }

    inline void operator()(FunctionMQMessage& message) { message.function_with_snapshot(snapshot_); }

    inline void operator()(HTTPRequestMQMessage& message) {
      message.http_function_with_snapshot(std::move(message.request), snapshot_);
    }

    inline void operator()(VizMQMessage& message) {
      // Retrieve the current images, read-lock-protected, no external notifications.
      const std::string image = visualization_.ImmutableScopedAccessor()->image;
      if (!image.empty()) {
        message.request(image, HTTPResponseCode.OK, "image/png");
      } else {
        message.request("Not ready yet.", HTTPResponseCode.BadRequest, "text/plain");
      }
    }

    inline void operator()(TickMQMessage& message) {
      const double t = static_cast<double>(Now());
      message.p_u_total.Publish(VizPoint<int>{t, static_cast<int>(snapshot_.box.users.size())});
      message.p_q_total.Publish(VizPoint<int>{t, static_cast<int>(snapshot_.box.questions.size())});
      message.p_e_15sec.Publish(VizPoint<int>{t, snapshot_.engagement.GetValueOverSlidingWindow(t)});
    }

    // TODO(dkorolev): Move to optimizing non-static function here.
    struct StaticFunctionData {
      // Number of users.
      size_t N;

      // AD[i][j] = { # of agreements, # of disagreements }.
      std::vector<std::vector<std::pair<size_t, size_t>>> AD;

      struct OutputPoint {
        double x;
        double y;
        const std::string& s;
      };

      std::vector<OutputPoint> data;

      template <typename X>
      static X2V<X> compute(const X& x) {
        typedef X2V<X> V;
        const auto& data = bricks::ThreadLocalSingleton<StaticFunctionData>();

        assert(x.size() == data.N * 2);  // Pairs of coordinates.

        // Prepare the input.
        std::vector<std::pair<V, V>> P(data.N);
        for (size_t i = 0; i < data.N; ++i) {
          P[i].first = x[i * 2];
          P[i].second = x[i * 2 + 1];
        }

        // Compute the cost function.
        V penalty = 0.0;
        const double agree_prior = 0.1;
        const double disagree_prior = 0.5;
        const double max_distance = 2.05;
        for (size_t i = 0; i + 1 < data.N; ++i) {
          for (size_t j = i + 1; j < data.N; ++j) {
            const V dx = P[j].first - P[i].first;
            const V dy = P[j].second - P[i].second;
            const V d = sqrt(dx * dx + dy * dy);
            penalty -= log(d) * (disagree_prior + data.AD[i][j].second);
            penalty -= log(1.0 - (d / max_distance)) * (agree_prior + data.AD[i][j].first);
          }
        }
        return penalty;
      }

      void Update(const Snapshot::Box& box) {
        auto& static_data = bricks::ThreadLocalSingleton<StaticFunctionData>();
        size_t& N = static_data.N;
        std::vector<std::vector<std::pair<size_t, size_t>>>& AD = static_data.AD;

        const double t = static_cast<double>(bricks::time::Now());
        std::cerr << "Optimizing.\n";

        data.clear();

        N = box.users.size();

        if (N) {
          std::map<std::string, size_t> uid_remap;
          for (size_t i = 0; i < N; ++i) {
            uid_remap[box.users[i]] = i;
          }

          AD = std::vector<std::vector<std::pair<size_t, size_t>>>(
              N, std::vector<std::pair<size_t, size_t>>(N, std::pair<size_t, size_t>(0u, 0u)));

          for (const auto qit : box.answers) {
            std::vector<std::string> clusters[2];  // Disagree, Agree.
            for (const auto uit : qit.second) {
              if (uit.second == schema::ANSWER::DISAGREE) {
                clusters[0].push_back(uit.first);
              } else if (uit.second == schema::ANSWER::AGREE) {
                clusters[1].push_back(uit.first);
              }
            }
            for (size_t c = 0; c < 2; ++c) {
              for (size_t i = 0; i + 1 < clusters[c].size(); ++i) {
                for (size_t j = i + 1; j < clusters[c].size(); ++j) {
                  ++AD[uid_remap[clusters[c][i]]][uid_remap[clusters[c][j]]].first;
                  ++AD[uid_remap[clusters[c][j]]][uid_remap[clusters[c][i]]].first;
                }
              }
            }
            if (!clusters[0].empty() && !clusters[1].empty()) {
              for (const auto& cit1 : clusters[0]) {
                for (const auto& cit2 : clusters[1]) {
                  ++AD[uid_remap[cit1]][uid_remap[cit2]].second;
                  ++AD[uid_remap[cit2]][uid_remap[cit1]].second;
                }
              }
            }
          }

          std::vector<double> x;
          for (size_t i = 0; i < N; ++i) {
            const double phi = M_PI * 2 * i / N;
            x.push_back(cos(phi));
            x.push_back(sin(phi));
          }

          for (size_t i = 0; i < N; ++i) {
            std::cerr << bricks::strings::Printf("P0 = { %+.3lf, %+.3lf }\n", x[i * 2], x[i * 2 + 1]);
          }

          fncas::OptimizerParameters params;
          params.SetValue("max_steps", 50);
          params.SetValue("bt_beta", 0.5);
          params.SetValue("grad_eps", 0.5);
          const auto result = fncas::ConjugateGradientOptimizer<StaticFunctionData>(params).Optimize(x);

          x = result.point;
          for (size_t i = 0; i < N; ++i) {
            std::cerr << bricks::strings::Printf("P1 = { %+.3lf, %+.3lf }\n", x[i * 2], x[i * 2 + 1]);
          }

          for (size_t i = 0; i < N; ++i) {
            std::cerr << bricks::strings::Printf("%10s", box.users[i].c_str());
            for (size_t j = 0; j < N; ++j) {
              std::cerr << bricks::strings::Printf(
                  "  %dA/%dD", static_cast<int>(AD[i][j].first), static_cast<int>(AD[i][j].second));
            }
            std::cerr << std::endl;
          }

          for (size_t i = 0; i < N; ++i) {
            data.push_back(OutputPoint{x[i * 2], x[i * 2 + 1], box.users[i]});
          }
        }
        std::cerr << bricks::strings::Printf("Optimization took %.2lf seconds.\n",
                                             1e-3 * (static_cast<double>(bricks::time::Now()) - t));
      }
    };

    static std::string RegenerateImage(const Snapshot::Box& box) {
      if (!box.users.empty()) {
        bricks::ThreadLocalSingleton<StaticFunctionData>().Update(box);

        using namespace bricks::gnuplot;
        const auto f = [](Plotter& p) {
          const auto& data = bricks::ThreadLocalSingleton<StaticFunctionData>().data;
          for (const auto& cit : data) {
            p(cit.x, cit.y, cit.s);
          }
        };

        // TODO(dkorolev): Research more on `pngcairo`. It does look better for the demo. :-)
        return GNUPlot()
            .ImageSize(400, 400)
            .NoTitle()
            .NoKey()
            .NoTics()
            .NoBorder()
            .Plot(WithMeta(f).AsLabels())
            .OutputFormat("pngcairo");
      } else {
        return "";
      }
    }

    void TriggerVisualizationUpdate() {
      visualization_.MutableUse([this](Visualization& visualization) {
        // Make a copy of `snapshot_.box` to work with.
        // And signal the image update thread that it now has a job.
        visualization.box = snapshot_.box;
        ++visualization.requested;
      });
    }

    // The thread in which model and visualizations updates are run. Objectives:
    // 1) Don't block the main thread while the model+visualization are being updated,
    // 2) Skip intermediate models, if user action(s) happen faster than the model is updated.
    void UpdateVisualizationThread() {
      while (true) {
        // Patiently wait for new user-generated data to update the model+visualization.
        visualization_.Wait([](const Visualization& v) { return v.done < v.requested; });
        // Work with the copy of the box.
        Visualization copy = *visualization_.ImmutableScopedAccessor();
        std::cerr << "Starting to process request " << copy.requested << std::endl;
        const double timestamp = static_cast<double>(bricks::time::Now());
        const std::string image = RegenerateImage(copy.box);
        visualization_.MutableUse([&copy, &image](Visualization& v) {
          v.image = image;
          // Update to the `requested` version which was actually processed.
          // This is the most concurrency-safe solution.
          v.done = copy.requested;
          std::cerr << "Processed request " << copy.requested << std::endl;
        });
        image_stream_.Publish(VizPoint<std::string>{timestamp, Printf("/viz.png?key=%lf", timestamp)});
      }
    }
  };

  // TODO(dkorolev): There should probably be a better, more Bricks-standard way to make use of a metronome.
  void MetronomeThread() {
    const MILLISECONDS_INTERVAL period = static_cast<MILLISECONDS_INTERVAL>(500);
    EPOCH_MILLISECONDS now = Now();
    while (true) {
      mq_.EmplaceMessage(new TickMQMessage(u_total_, q_total_, e_15sec_));
      bricks::time::SleepUntil(now + period);
      now = Now();
    }
  }

 private:
  const std::string& demo_id_;

  sherlock::StreamInstance<VizPoint<int>> u_total_;
  sherlock::StreamInstance<VizPoint<int>> q_total_;
  sherlock::StreamInstance<VizPoint<int>> e_15sec_;
  sherlock::StreamInstance<VizPoint<std::string>> image_;

  Consumer consumer_;
  MMQ<Consumer, std::unique_ptr<schema::Base>> mq_;

  std::thread metronome_thread_;

  Cruncher() = delete;
  Cruncher(const Cruncher&) = delete;
  void operator=(const Cruncher&) = delete;
  Cruncher(Cruncher&&) = delete;
  void operator=(Cruncher&&) = delete;
};

class MixpanelUploader final {
 public:
  struct MixpanelEvent {
    struct User {
      struct Properties {
        // (reserved) The Mixpanel project token.
        std::string token;

        // (reserved) The identifier of the user who caused the event to happen.
        std::string distinct_id;

        // (reserved) The time of the event, in seconds.
        uint64_t time;

        template <typename A>
        void serialize(A& ar) {
          ar(CEREAL_NVP(token), CEREAL_NVP(distinct_id), CEREAL_NVP(time));
        }
      };

      std::string event;
      Properties properties;

      User(const std::string& token, const schema::UserRecord& u) {
        event = "User";
        properties.token = token;
        properties.distinct_id = u.uid;
        properties.time = static_cast<uint64_t>(u.ms) / 1000;
      }

      template <typename A>
      void serialize(A& ar) {
        ar(CEREAL_NVP(event), CEREAL_NVP(properties));
      }
    };

    struct Answer {
      struct Properties {
        // (reserved) The Mixpanel project token.
        std::string token;

        // (reserved) The identifier of the user who caused the event to happen.
        std::string distinct_id;

        // (reserved) The time of the event, in seconds.
        uint64_t time;

        // Question identifier.
        schema::QID qid;

        // Answer identifier.
        schema::ANSWER answer;

        template <typename A>
        void serialize(A& ar) {
          ar(CEREAL_NVP(token),
             CEREAL_NVP(distinct_id),
             CEREAL_NVP(time),
             cereal::make_nvp("Question", static_cast<size_t>(qid)),
             cereal::make_nvp("Answer", static_cast<int>(answer)));
        }
      };

      std::string event;
      Properties properties;

      Answer(const std::string& token, const schema::AnswerRecord& a) {
        event = "Answer";
        properties.token = token;
        properties.distinct_id = a.uid;
        properties.time = static_cast<uint64_t>(a.ms) / 1000;
        properties.qid = a.qid;
        properties.answer = a.answer;
      }

      template <typename A>
      void serialize(A& ar) {
        ar(CEREAL_NVP(event), CEREAL_NVP(properties));
      }
    };

  };  // struct MixpanelEvent

  MixpanelUploader(const std::string& demo_id, const std::string& mixpanel_token)
      : demo_id_(demo_id), mixpanel_token_(mixpanel_token) {}

  inline bool Entry(const std::unique_ptr<schema::Base>& entry, size_t index, size_t total) {
    static_cast<void>(index);
    static_cast<void>(total);

    struct types {
      typedef schema::Base base;
      typedef std::tuple<schema::UserRecord, schema::AnswerRecord> derived_list;
      typedef bricks::rtti::RuntimeTupleDispatcher<base, derived_list> dispatcher;
    };
    types::dispatcher::DispatchCall(*entry, *this);

    return true;
  }

  inline void Terminate() { std::cerr << '@' << demo_id_ << " MixpanelUploader is done.\n"; }

  inline void operator()(schema::Base&) {
    // Sink for ignored events; currently `Question`-s.
  }

  template <typename T>
  inline void PushMixpanelEvent(T&& event) {
    const std::string json = bricks::cerealize::MultiKeyJSON(event);
    std::cerr << '@' << demo_id_ << " MixpanelUploader Event: " << json << std::endl;
    const std::string base64_json = bricks::cerealize::Base64Encode(json);
    // WORKAROUND(sompylasar): Not using `https://`, could not send HTTPS request.
    const std::string mixpanel_request = "http://api.mixpanel.com/track?data=" + base64_json;
    std::cerr << '@' << demo_id_ << " MixpanelUploader Request: " << mixpanel_request << std::endl;
    if (mixpanel_token_.empty()) {
      std::cerr << '@' << demo_id_ << " MixpanelUploader Empty token, not sending." << std::endl;
      return;
    }
    auto response = HTTP(GET(mixpanel_request));
    std::cerr << '@' << demo_id_ << " MixpanelUploader Response: HTTP " << static_cast<int>(response.code)
              << " \"" << response.body << "\"" << std::endl;
  }

  inline void operator()(schema::UserRecord& u) { PushMixpanelEvent(MixpanelEvent::User(mixpanel_token_, u)); }

  inline void operator()(schema::AnswerRecord& a) {
    PushMixpanelEvent(MixpanelEvent::Answer(mixpanel_token_, a));
  }

 private:
  const std::string& demo_id_;
  const std::string& mixpanel_token_;

  MixpanelUploader() = delete;
  MixpanelUploader(const MixpanelUploader&) = delete;
  void operator=(const MixpanelUploader&) = delete;
  MixpanelUploader(MixpanelUploader&&) = delete;
  void operator=(MixpanelUploader&&) = delete;
};

struct Controller {
 public:
  explicit Controller(int port, const std::string& demo_id, const std::string& mixpanel_token, db::Storage* db)
      : port_(port),
        demo_id_(demo_id),
        mixpanel_token_(mixpanel_token),
        html_header_(FileSystem::ReadFileAsString(FileSystem::JoinPath("static", "actions_header.html"))),
        html_footer_(FileSystem::ReadFileAsString(FileSystem::JoinPath("static", "actions_footer.html"))),
        db_(db),
        cruncher_(port_, demo_id_),
        cruncher_scope_(db_->Subscribe(cruncher_)),
        mixpanel_uploader_(demo_id_, mixpanel_token_),
        mixpanel_uploader_scope_(db->Subscribe(mixpanel_uploader_)) {
    // The main controller page.
    HTTP(port_).Register("/" + demo_id_ + "/a/", std::bind(&Controller::Actions, this, std::placeholders::_1));
    HTTP(port_).Register("/" + demo_id_ + "/a", [this](Request r) {
      r("", HTTPResponseCode.Found, "text/html", HTTPHeaders().Set("Location", "/" + demo_id_ + "/a/"));
    });

    // Make the storage-level stream accessible to the outer world via PubSub.
    HTTP(port_).Register("/" + demo_id_ + "/a/raw", std::ref(*db_));

    // Pre-populate a few users, questions and answers to start from.
    db->DoAddUser("alice", Now() - MILLISECONDS_INTERVAL(9000));
    db->DoAddUser("barbie", Now() - MILLISECONDS_INTERVAL(8000));
    db->DoAddUser("cindy", Now() - MILLISECONDS_INTERVAL(7000));
    db->DoAddUser("daphne", Now() - MILLISECONDS_INTERVAL(6000));
    db->DoAddUser("eve", Now() - MILLISECONDS_INTERVAL(5000));
    db->DoAddUser("fiona", Now() - MILLISECONDS_INTERVAL(4000));
    db->DoAddUser("gina", Now() - MILLISECONDS_INTERVAL(3000));
    db->DoAddUser("helen", Now() - MILLISECONDS_INTERVAL(2000));
    db->DoAddUser("irene", Now() - MILLISECONDS_INTERVAL(1000));

    const auto vi = db->DoAddQuestion("Vi is the best text editor.", Now() - MILLISECONDS_INTERVAL(4500)).qid;
    const auto weed = db->DoAddQuestion("Marijuana should be legal.", Now() - MILLISECONDS_INTERVAL(3500)).qid;
    const auto bubble = db->DoAddQuestion("We are in the bubble.", Now() - MILLISECONDS_INTERVAL(2500)).qid;
    const auto movies = db->DoAddQuestion("Movies are getting worse.", Now() - MILLISECONDS_INTERVAL(1500)).qid;

    db->DoAddAnswer("alice", vi, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("alice", weed, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("barbie", movies, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("barbie", bubble, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("cindy", vi, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("cindy", weed, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("cindy", bubble, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("cindy", movies, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("daphne", vi, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("daphne", weed, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("daphne", bubble, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("daphne", movies, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("eve", weed, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("eve", movies, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("fiona", weed, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("fiona", movies, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("gina", weed, schema::ANSWER::AGREE, Now());
    db->DoAddAnswer("gina", movies, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("helen", weed, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("helen", movies, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("irene", weed, schema::ANSWER::DISAGREE, Now());
    db->DoAddAnswer("irene", movies, schema::ANSWER::DISAGREE, Now());
  }

  void Actions(Request r) {
    // This request goes through the Cruncher's message queue to ensure no concurrent access to the snapshot.
    cruncher_.ServeRequestWithSnapshot(std::move(r), [this](Request r, Snapshot& snapshot) {
      std::ostringstream table;
      table << "<tr><td></td>";
      for (const auto& u : snapshot.box.users) {
        table << "<td align=center><b>" << u << "</b></td>";
      }
      table << "<tr>\n";
      for (size_t qi = 0; qi < snapshot.box.questions.size(); ++qi) {
        const auto& q = snapshot.box.questions[qi];
        table << "<tr><td align=right><b>" << q << "</b></td>";
        std::map<schema::UID, schema::ANSWER>& current_answers =
            snapshot.box.answers[static_cast<schema::QID>(qi + 1)];
        for (const auto& u : snapshot.box.users) {
          table << "<td align=center>";
          struct VTC {  // VTC = { Value, Text, Color }.
            int value;
            const char* text;
            const char* color;
          };
          static constexpr VTC options[3] = {{-1, "No", "red"}, {0, "N/A", "gray"}, {+1, "Yes", "green"}};
          const int current_answer = static_cast<int>(current_answers[u]);
          for (size_t i = 0; i < 3; ++i) {
            if (i) {
              table << " | ";
            }
            if (options[i].value != current_answer) {
              table << Printf("<a href='add_answer?uid=%s&qid=%d&answer=%d'>%s</a>",
                              u.c_str(),
                              static_cast<int>(qi + 1),
                              options[i].value,
                              options[i].text);
            } else {
              table << Printf("<b><font color=%s>%s</font></b>", options[i].color, options[i].text);
            }
          }
          table << "</td>";
        }
        table << "</tr>\n";
      }
      r(html_header_ + table.str() + html_footer_, HTTPResponseCode.OK, "text/html");
    });
  }

 private:
  const int port_;
  const std::string demo_id_;
  const std::string mixpanel_token_;

  const std::string html_header_;
  const std::string html_footer_;

  db::Storage* db_;  // `db_` is owned by the creator of the instance of `Controller`.
  Cruncher cruncher_;
  typename sherlock::StreamInstance<std::unique_ptr<schema::Base>>::template ListenerScope<Cruncher>
      cruncher_scope_;
  MixpanelUploader mixpanel_uploader_;
  typename sherlock::StreamInstance<std::unique_ptr<schema::Base>>::template ListenerScope<MixpanelUploader>
      mixpanel_uploader_scope_;

  Controller() = delete;
};

int main() {
  const int port = FLAGS_port;

  // Create and redirect to a new demo when POST-ed onto `/new`.
  HTTP(port).Register("/new", [&port](Request r) {
    if (r.method == "POST") {
      try {
        using bricks::net::url::URL;
        std::cerr << "New demo requested: \"" << r.body << "\"" << std::endl;
        // HACK(sompylasar): Parse the URL-encoded body as a query-string.
        URL body_parsed = URL("/?" + r.body);
        std::string mixpanel_token = bricks::strings::Trim(body_parsed.query.get("mixpanel_token", ""));
        std::cerr << "Mixpanel token: \"" << mixpanel_token << "\"" << std::endl;
        uint64_t salt = static_cast<uint64_t>(Now());
        // Randomly generated `demo_id` w/o safety checking. -- D.K.
        std::string demo_id = "";
        for (size_t i = 0; i < 5; ++i) {
          demo_id = std::string(1, ('a' + (salt % 26))) + demo_id;  // "MSB" first ordering.
          salt /= 26;
        }
        auto demo = new db::Storage(port, demo_id);                             // Lives forever. -- D.K.
        auto controller = new Controller(port, demo_id, mixpanel_token, demo);  // Lives forever. -- D.K.
        static_cast<void>(controller);
        r("", HTTPResponseCode.Found, "text/html", HTTPHeaders().Set("Location", "/" + demo_id + "/a/"));
      } catch (const bricks::Exception& e) {
        std::cerr << "Demo creation exception: " << e.What() << std::endl;
        throw;
      }
    } else {
      r(bricks::net::DefaultMethodNotAllowedMessage(), HTTPResponseCode.MethodNotAllowed, "text/html");
    }
  });

  // Landing page.
  const std::string dir = "static/";
  HTTP(port).ServeStaticFilesFrom(dir, "/static/");
  HTTP(port).Register(
      "/",
      new bricks::net::api::StaticFileServer(
          bricks::FileSystem::ReadFileAsString(FileSystem::JoinPath(dir, "landing.html")), "text/html"));

  std::cerr << "Serving at port " << port << ".\n";

  // Run forever.
  HTTP(port).Join();
}
