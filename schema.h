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

#ifndef SCHEMA_H
#define SCHEMA_H

#include "../Bricks/port.h"

#include <string>

#include "../Bricks/cerealize/cerealize.h"
#include "../Bricks/time/chrono.h"

// Schema for storage records and low-level API calls.
namespace schema {

typedef std::string UID;                                        // User ID, just use their name.
enum class QID : size_t { NONE = 0 };                           // Question ID, 1-based, 0 is unused.
enum class ANSWER : int { DISAGREE = -1, NA = 0, AGREE = +1 };  // Answer, one of { AGREE, DISAGREE, NA }.

struct Base {
  virtual bricks::time::EPOCH_MILLISECONDS ExtractTimestamp() const {
    // TODO(dkorolev): Something smarter.
    throw false;
  }
  virtual ~Base() = default;
  template <typename A>
  void serialize(A&) {}
};

struct Record : Base {
  bricks::time::EPOCH_MILLISECONDS ms;
  template <typename A>
  void serialize(A& ar) {
    Base::serialize(ar);
    ar(CEREAL_NVP(ms));
  }
  virtual bricks::time::EPOCH_MILLISECONDS ExtractTimestamp() const override { return ms; }
};

struct UserRecord : Record {
  UID uid;
  template <typename A>
  void serialize(A& ar) {
    Record::serialize(ar);
    ar(CEREAL_NVP(uid));
  }
};

struct QuestionRecord : Record {
  QID qid;
  std::string text;
  template <typename A>
  void serialize(A& ar) {
    Record::serialize(ar);
    ar(CEREAL_NVP(qid), CEREAL_NVP(text));
  }
};

// TODO(dkorolev): Perhaps add a unit test for this one.
struct AnswerRecord : Record {
  UID uid;
  QID qid;
  ANSWER answer;
  template <typename A>
  void serialize(A& ar) {
    Record::serialize(ar);
    ar(CEREAL_NVP(uid), CEREAL_NVP(qid), CEREAL_NVP(answer));
  }
};

}  // namespace schema

/*
// The following lines should be put into a C++ source file to avoid breaking the ODR. -- D.K.
CEREAL_REGISTER_TYPE_WITH_NAME(schema::Record, "0");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::UserRecord, "U");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::QuestionRecord, "Q");
CEREAL_REGISTER_TYPE_WITH_NAME(schema::AnswerRecord, "A");
*/

#endif  // SCHEMA_H
