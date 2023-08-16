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

#define BRICKS_MOCK_TIME

#include "../../Bricks/port.h"

#include <string>

#include "../db.h"
#include "../schema.h"

CEREAL_REGISTER_TYPE_WITH_NAME(schema::Record, "0");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::UserRecord, "U");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::QuestionRecord, "Q");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::AnswerRecord, "A");

// TODO(dkorolev): Move this into Bricks.
#include "../bricks-cerealize-multikeyjson.h"
#include "../bricks-cerealize-base64.h"

#include "../../Bricks/dflags/dflags.h"
#include "../../Bricks/3party/gtest/gtest-main-with-dflags.h"

using bricks::Singleton;
using bricks::strings::Printf;

DEFINE_int32(test_port, 8091, "Local port to use for the test.");

struct ListenOnTestPort {
  ListenOnTestPort() {
    HTTP(FLAGS_test_port).Register("/", [](Request r) { r("I'm listening, baby.\n"); });
  }
};

struct UnitTestStorageListener {
  std::atomic_size_t n;
  std::string data;
  UnitTestStorageListener() : n(0u) {}
  inline bool Entry(std::unique_ptr<schema::Base>& entry, size_t index, size_t total) {
    static_cast<void>(index);
    static_cast<void>(total);
    data += JSON(entry, "record") + "\n";
    ++n;
    return true;
  }
  inline void Terminate() {
    data += "DONE\n";
    ++n;
  }
};

TEST(AgreeDisagreeDemo, EndpointsAndScope) {
  const std::string url_prefix = Printf("http://localhost:%d", FLAGS_test_port);
  // `/test1` is inactive. Ensure that an HTTP server is listening on the port though.
  Singleton<ListenOnTestPort>();
  EXPECT_EQ(404, static_cast<int>(HTTP(GET(url_prefix + "/test1")).code));
  {
    db::Storage storage(FLAGS_test_port, "test1");
    // `/test1` is available in the scope of `Storage("test1");`.
    EXPECT_EQ(200, static_cast<int>(HTTP(GET(url_prefix + "/test1")).code));
  }
  // `/test1` is inactive again as `Storage("test1");` gets out of scope.
  EXPECT_EQ(404, static_cast<int>(HTTP(GET(url_prefix + "/test1")).code));
}

TEST(AgreeDisagreeDemo, Questions) {
  db::Storage storage(FLAGS_test_port, "test2");

  UnitTestStorageListener listener;
  auto listener_scope = storage.Subscribe(listener);

  const std::string url_prefix = Printf("http://localhost:%d", FLAGS_test_port);
  // The question with QID=1 does not exist.
  EXPECT_EQ(404, static_cast<int>(HTTP(GET(url_prefix + "/test2/q?qid=1")).code));
  // A question can be added and gets a QID of 1.
  EXPECT_EQ(0u, listener.n);
  bricks::time::SetNow(bricks::time::EPOCH_MILLISECONDS(1001));
  const auto added = HTTP(POST(url_prefix + "/test2/q?text=Why%3F", ""));
  EXPECT_EQ(200, static_cast<int>(added.code));
  EXPECT_EQ("{\"question\":{\"ms\":1001,\"qid\":1,\"text\":\"Why?\"}}\n", added.body);
  // A new question with the same text can not be added.
  EXPECT_EQ(400, static_cast<int>(HTTP(POST(url_prefix + "/test2/q?text=Why%3F", "")).code));
  // A question with QID of 1 can be retrieved now.
  const auto retrieved = HTTP(GET(url_prefix + "/test2/q?qid=1"));
  EXPECT_EQ(200, static_cast<int>(retrieved.code));
  EXPECT_EQ("{\"value0\":{\"ms\":1001,\"qid\":1,\"text\":\"Why?\"}}\n", retrieved.body);

  // Ensure that the question is processed as it reaches the listener stream.
  while (listener.n != 1) {
    ;  // Spin lock;
  }

  EXPECT_EQ(
      "{\"record\":{\"polymorphic_id\":2147483649,\"polymorphic_name\":\"Q\",\"ptr_wrapper\":"
      "{\"valid\":1,\"data\":{\"ms\":1001,\"qid\":1,\"text\":\"Why?\"}}"
      "}}\n",
      listener.data);

  // TODO(dkorolev): Fix Sherlock wrt joining streams.
  // listener_scope.Join();
}

TEST(AgreeDisagreeDemo, Users) {
  db::Storage storage(FLAGS_test_port, "test3");
  const std::string url_prefix = Printf("http://localhost:%d", FLAGS_test_port);
  // The user "adam" does not exist.
  EXPECT_EQ(404, static_cast<int>(HTTP(GET(url_prefix + "/test3/u?uid=adam")).code));
  // The user "adam" can be added.
  const auto added = HTTP(POST(url_prefix + "/test3/u?uid=adam", ""));
  EXPECT_EQ(200, static_cast<int>(added.code));
  EXPECT_EQ("{\"user\":{\"ms\":1001,\"uid\":\"adam\"}}\n", added.body);
  // The user "adam" cannot be re-added.
  EXPECT_EQ(400, static_cast<int>(HTTP(POST(url_prefix + "/test3/u?uid=adam", "")).code));
  // The user "adam" exists now.
  EXPECT_EQ(200, static_cast<int>(HTTP(GET(url_prefix + "/test3/u?uid=adam")).code));
}

struct MultiKeyJSONTestObject {
  struct InnerObject {
    int inner_key = 1;

    template <typename A>
    void serialize(A& ar) {
      ar(CEREAL_NVP(inner_key));
    }
  };

  std::string key1 = "value1";
  InnerObject key2;

  template <typename A>
  void serialize(A& ar) {
    ar(CEREAL_NVP(key1), CEREAL_NVP(key2));
  }
};

TEST(BricksCerealizeMultiKeyJSON, SmokeTest) {
  // TODO(dkorolev): Please make `MultiKeyJSON` work with `const`.
  MultiKeyJSONTestObject object;
  const std::string json = bricks::cerealize::MultiKeyJSON(object);
  EXPECT_EQ("{\"key1\":\"value1\",\"key2\":{\"inner_key\":1}}", json);
}

TEST(BricksCerealizeBase64Encode, SmokeTest) {
  EXPECT_EQ("MTIzNDU=", bricks::cerealize::Base64Encode("12345"));
  EXPECT_EQ("NzY1NFh5Wg==", bricks::cerealize::Base64Encode("7654XyZ"));
}
