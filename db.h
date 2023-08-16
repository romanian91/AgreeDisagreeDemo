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

#ifndef DB_H
#define DB_H

#include "../Bricks/port.h"

#include <vector>
#include <set>
#include <string>

#include "schema.h"

#include "../Bricks/cerealize/cerealize.h"
#include "../Bricks/time/chrono.h"
#include "../Bricks/net/api/api.h"
#include "../Bricks/dflags/dflags.h"

#include "../Sherlock/sherlock.h"

namespace db {

// The instance of the `Storage` class governs low-level API HTTP endpoints
// and the Sherlock stream for the instance of the user-facing demo.
//
// One instance of `Storage` exists per one instance of the user-facing demo endpoint.

class Storage final {
 public:
  // Registers HTTP endpoints for the provided client name.
  // Ensures that questions indexing will start from 1 by adding a dummy question with index 0.
  Storage(int port, const std::string& client_name)
      : port_(port),
        client_name_(client_name),
        sherlock_stream_(sherlock::Stream<std::unique_ptr<schema::Base>>(client_name + "_db", "record")),
        questions_({schema::QuestionRecord()}),
        questions_reverse_index_({""}) {
    HTTP(port_).Register("/" + client_name_, [](Request r) { r("OK\n"); });
    HTTP(port_).Register("/" + client_name_ + "/q", std::bind(&Storage::HandleQ, this, std::placeholders::_1));
    HTTP(port_).Register("/" + client_name_ + "/u", std::bind(&Storage::HandleU, this, std::placeholders::_1));
    // TODO(dkorolev): POST "/a"?
    HTTP(port_).Register("/" + client_name_ + "/a/add_question",
                         std::bind(&Storage::HandleAddQ, this, std::placeholders::_1));
    HTTP(port_).Register("/" + client_name_ + "/a/add_user",
                         std::bind(&Storage::HandleAddU, this, std::placeholders::_1));
    HTTP(port_).Register("/" + client_name_ + "/a/add_answer",
                         std::bind(&Storage::HandleAddA, this, std::placeholders::_1));
  }

  // Unregisters HTTP endpoints.
  ~Storage() {
    HTTP(port_).UnRegister("/" + client_name_);
    HTTP(port_).UnRegister("/" + client_name_ + "/q");
    HTTP(port_).UnRegister("/" + client_name_ + "/u");
    HTTP(port_).UnRegister("/" + client_name_ + "/a/add_question");
    HTTP(port_).UnRegister("/" + client_name_ + "/a/add_user");
    HTTP(port_).UnRegister("/" + client_name_ + "/a/add_answer");
  }

  // Sherlock stream access.
  template <typename F>
  typename sherlock::StreamInstanceImpl<std::unique_ptr<schema::Base>>::template ListenerScope<F> Subscribe(
      F& listener) {
    return sherlock_stream_.Subscribe(listener);
  }

  template <typename F>
  typename sherlock::StreamInstanceImpl<std::unique_ptr<schema::Base>>::template ListenerScope<F> Subscribe(
      std::unique_ptr<F> listener) {
    return sherlock_stream_.Subscribe(std::move(listener));
  }

  // API implementation.
  // Bloated a bit for easier demonstration. -- D.K.
  const schema::QuestionRecord& DoAddQuestion(const std::string& text,
                                              bricks::time::EPOCH_MILLISECONDS timestamp) {
    const schema::QID qid = static_cast<schema::QID>(questions_.size());
    questions_.push_back(schema::QuestionRecord());
    schema::QuestionRecord& record = questions_.back();
    record.ms = timestamp;
    record.qid = qid;
    record.text = text;
    questions_reverse_index_.insert(text);
    sherlock_stream_.Publish(record);
    return record;
  }

  const schema::UserRecord& DoAddUser(const schema::UID& uid, bricks::time::EPOCH_MILLISECONDS timestamp) {
    schema::UserRecord& record = users_[uid];
    record.ms = timestamp;
    record.uid = uid;
    sherlock_stream_.Publish(record);
    return record;
  }

  schema::AnswerRecord DoAddAnswer(const schema::UID& uid,
                                   schema::QID qid,
                                   schema::ANSWER answer,
                                   bricks::time::EPOCH_MILLISECONDS timestamp) {
    schema::AnswerRecord record;
    record.ms = timestamp;
    record.uid = uid;
    record.qid = qid;
    record.answer = answer;
    sherlock_stream_.Publish(record);
    return record;
  }

  void operator()(Request r) { sherlock_stream_(std::move(r)); }

 private:
  // Retrieves or creates questions.
  void HandleQ(Request r) {
    if (r.method == "GET") {
      const schema::QID qid = static_cast<schema::QID>(atoi(r.url.query["qid"].c_str()));
      if (qid == schema::QID::NONE) {
        r("NEED QID\n", HTTPResponseCode.BadRequest);
      } else if (static_cast<size_t>(qid) >= questions_.size()) {
        r("QUESTION NOT FOUND\n", HTTPResponseCode.NotFound);
      } else {
        r(questions_[static_cast<size_t>(qid)]);
      }
    } else if (r.method == "POST") {
      HandleAddQ(std::move(r));
    } else {
      r("METHOD NOT ALLOWED\n", HTTPResponseCode.MethodNotAllowed);
    }
  }

  void HandleAddQ(Request r) {
    const std::string text = r.url.query["text"];
    if (text.empty()) {
      r("NEED TEXT\n", HTTPResponseCode.BadRequest);
    } else if (questions_reverse_index_.count(text)) {
      r("DUPLICATE QUESTION\n", HTTPResponseCode.BadRequest);
    } else {
      RespondWith(std::move(r), DoAddQuestion(text, r.timestamp), "question");
    }
  }

  // Retrieves or creates users. Factored out to allow GET-s as well, for simpler "Web" UX.
  void HandleU(Request r) {
    const schema::UID uid = r.url.query["uid"];
    if (uid.empty()) {
      r("NEED UID\n", HTTPResponseCode.BadRequest);
    } else {
      if (r.method == "GET") {
        const auto cit = users_.find(uid);
        if (cit != users_.end()) {
          r(cit->second, "user");
        } else {
          r("USER NOT FOUND\n", HTTPResponseCode.NotFound);
        }
      } else if (r.method == "POST") {
        HandleAddU(std::move(r));
      } else {
        r("METHOD NOT ALLOWED\n", HTTPResponseCode.MethodNotAllowed);
      }
    }
  }

  void HandleAddU(Request r) {
    const schema::UID uid = r.url.query["uid"];
    if (uid.empty()) {
      r("NEED UID\n", HTTPResponseCode.BadRequest);
    } else {
      if (users_.count(uid)) {
        r("USER ALREADY EXISTS\n", HTTPResponseCode.BadRequest);
      } else {
        RespondWith(std::move(r), DoAddUser(uid, r.timestamp), "user");
      }
    }
  }

  // TODO(dkorolev): HandleA()?
  void HandleAddA(Request r) {
    const schema::UID uid = r.url.query["uid"];
    const schema::QID qid = static_cast<schema::QID>(atoi(r.url.query["qid"].c_str()));
    const int answer_as_int = static_cast<int>(atoi(r.url.query["answer"].c_str()));
    const schema::ANSWER answer =
        static_cast<schema::ANSWER>([](int x) { return x ? (x > 0 ? +1 : -1) : 0; }(answer_as_int));
    if (uid.empty()) {
      r("NEED UID\n", HTTPResponseCode.BadRequest);
    } else if (!users_.count(uid)) {
      r("USER DOES NOT EXISTS\n", HTTPResponseCode.BadRequest);
    } else if (qid == schema::QID::NONE) {
      r("NEED QID\n", HTTPResponseCode.BadRequest);
    } else if (static_cast<size_t>(qid) >= questions_.size()) {
      r("QUESTION DOES NOT EXISTS\n", HTTPResponseCode.BadRequest);
    } else {
      RespondWith(std::move(r), DoAddAnswer(uid, qid, answer, r.timestamp), "answer");
    }
  }

  template <typename T>
  static void RespondWith(Request r, T&& json, const char* json_title) {
    if (r.method == "POST") {
      r(json, json_title);
    } else {
      const std::string html =
          "<!doctype html>\n"
          "<head><meta http-equiv='refresh' content='3;URL=.'></head>\n"
          "<table border=1><tr><td><pre>" +
          JSON(json, json_title) +
          "</pre></td></tr></table>\n"
          "<br>You will be taken back in a blast.<br>";
      r(html, HTTPResponseCode.OK, "text/html");
    }
  }

  const int port_;
  const std::string client_name_;

  sherlock::StreamInstance<std::unique_ptr<schema::Base>> sherlock_stream_;

  std::vector<schema::QuestionRecord> questions_;
  std::set<std::string> questions_reverse_index_;  // To disallow duplicate questions.

  std::map<schema::UID, schema::UserRecord> users_;

  Storage() = delete;
  Storage(const Storage&) = delete;
  Storage(Storage&&) = delete;
  void operator=(const Storage&) = delete;
  void operator=(Storage&&) = delete;
};

}  // namespace db

#endif  // DB_H
