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

#ifndef DASHBOARD_H
#define DASHBOARD_H

#include "../Bricks/port.h"

#include <string>
#include <vector>

#include "../Bricks/cerealize/cerealize.h"
#include "../Bricks/strings/printf.h"
#include "../Bricks/file/file.h"

namespace dashboard {

struct Config {
  const std::string layout_url;

  // The list of domains that resolve to the backend which serves the layout and streams.
  std::vector<std::string> data_hostnames;

  // The static template.
  std::string dashboard_template;

  explicit Config(const std::string& layout_url, const std::string& dashboard_template)
      : layout_url(layout_url), dashboard_template(dashboard_template) {
    // Assume 'TLD=tailproduce.io' of some sort ...
    const char* tld_env = std::getenv("TLD");
    // ... or require a local '/etc/hosts' tweak.
    const std::string tld = tld_env ? tld_env : "knowsheet.local";
    for (int i = 0; i < 10; ++i) {
      data_hostnames.push_back(bricks::strings::Printf("d%d.%s", i, tld.c_str()));
    }
  }

  template <typename A>
  void save(A& ar) const {
    ar(CEREAL_NVP(layout_url), CEREAL_NVP(data_hostnames), CEREAL_NVP(dashboard_template));
  }
};

struct PlotMeta {
  struct Options {
    std::string caption = "<CAPTION>";
    std::string color = "blue";
    // double min = -5;
    // double max = 25;
    double time_interval = 30000;
    size_t n_min = 2;

    template <typename A>
    void save(A& ar) const {
      // TODO(dkorolev): Make serialization of `min/max` conditional on their equality.
      ar(cereal::make_nvp("header_text", caption),
         CEREAL_NVP(color),
         // CEREAL_NVP(min),
         // CEREAL_NVP(max),
         CEREAL_NVP(time_interval),
         CEREAL_NVP(n_min));
    }
  };

  // The `data_url` is relative to the `layout_url`.
  std::string data_url;
  std::string visualizer_name = "plot-visualizer";
  Options options;

  template <typename A>
  void save(A& ar) const {
    ar(CEREAL_NVP(data_url), CEREAL_NVP(visualizer_name), cereal::make_nvp("visualizer_options", options));
  }
};

struct ImageMeta {
  struct Options {
    std::string header_text = "<CAPTION>";
    std::string empty_text = "Loading...";
    double time_interval = 60000;  //  Although "1" should work here. -- D.K.
    size_t n_min = 1;

    template <typename A>
    void save(A& ar) const {
      ar(CEREAL_NVP(header_text), CEREAL_NVP(empty_text), CEREAL_NVP(time_interval), CEREAL_NVP(n_min));
    }
  };

  // The `data_url` is relative to the `layout_url`.
  std::string data_url = "/pic_data";
  std::string visualizer_name = "image-visualizer";
  Options options;

  template <typename A>
  void save(A& ar) const {
    ar(CEREAL_NVP(data_url), CEREAL_NVP(visualizer_name), cereal::make_nvp("visualizer_options", options));
  }
};

namespace layout {

// TODO(dkorolev): Think of making it impossible to create a row or rows or a col of cols.

struct Cell {
  // The `meta_url` is relative to the `layout_url`.
  std::string meta_url;

  Cell(const std::string& meta_url = "") : meta_url(meta_url) {}

  template <typename A>
  void save(A& ar) const {
    ar(CEREAL_NVP(meta_url));
  }
};

struct Layout;

struct Row {
  std::vector<Layout> data;
  Row(std::initializer_list<Layout> data) : data(data) {}
};

struct Col {
  std::vector<Layout> data;
  Col(std::initializer_list<Layout> data) : data(data) {}
};

struct Layout {
  std::vector<Layout> row;
  std::vector<Layout> col;
  Cell cell;

  Layout() {}
  Layout(const Row& row) : row(row.data) {}
  Layout(const Col& col) : col(col.data) {}
  Layout(const Cell& cell) : cell(cell) {}

  template <typename A>
  void save(A& ar) const {
    if (!row.empty()) {
      ar(CEREAL_NVP(row));
    } else if (!col.empty()) {
      ar(CEREAL_NVP(col));
    } else {
      ar(CEREAL_NVP(cell));
    }
  }
};

}  // namespace layout

using layout::Layout;

}  // namespace dashboard

#endif  // DASHBOARD_H
