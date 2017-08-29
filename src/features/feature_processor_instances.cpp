// This file is part of NameTag <http://github.com/ufal/nametag/>.
//
// Copyright 2016 Institute of Formal and Applied Linguistics, Faculty of
// Mathematics and Physics, Charles University in Prague, Czech Republic.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <fstream>

#include "feature_processor.h"
#include "unilib/unicode.h"
#include "unilib/utf8.h"
#include "utils/parse_int.h"
#include "utils/split.h"
#include "utils/url_detector.h"

namespace ufal {
namespace nametag {

// Helper functions defined as macros so that they can access arguments without passing them
#define apply_in_window(I, Feature) apply_in_range(I, Feature, -window, window)

#define apply_in_range(I, Feature, Left, Right) {                                                   \
  ner_feature _feature = (Feature);                                                                 \
  if (_feature != ner_feature_unknown)                                                              \
    for (int _w = int(I) + (Left) < 0 ? 0 : int(I) + (Left),                                        \
           _end = int(I) + (Right) + 1 < int(sentence.size) ? int(I) + (Right) + 1 : sentence.size; \
         _w < _end; _w++)                                                                           \
      sentence.features[_w].emplace_back(_feature + _w - int(I));                                   \
}

#define apply_outer_words_in_window(Feature) {                   \
  ner_feature _outer_feature = (Feature);                        \
  if (_outer_feature != ner_feature_unknown)                     \
    for (int _i = 1; _i <= window; _i++) {                       \
        apply_in_window(-_i, _outer_feature);                    \
        apply_in_window(sentence.size - 1 + _i, _outer_feature); \
    }                                                            \
}

#define lookup_empty() /* lookup(string()) always returns */(window)


//////////////////////////////////////////////////////////////
// Feature processor instances (ordered lexicographically) //
//////////////////////////////////////////////////////////////
namespace feature_processors {

// BrownClusters
class brown_clusters : public feature_processor {
 public:
  virtual bool parse(int window, const vector<string>& args, entity_map& entities,
                     ner_feature* total_features, const nlp_pipeline& pipeline) override {
    if (!feature_processor::parse(window, args, entities, total_features, pipeline)) return false;
    if (args.size() < 1) return cerr << "BrownCluster requires a cluster file as the first argument!" << endl, false;

    ifstream in(args[0]);
    if (!in.is_open()) return cerr << "Cannot open Brown clusters file '" << args[0] << "'!" << endl, false;

    vector<size_t> substrings;
    substrings.emplace_back(string::npos);
    for (unsigned i = 1; i < args.size(); i++) {
      int len = parse_int(args[i].c_str(), "BrownCluster_prefix_length");
      if (len <= 0)
        return cerr << "Wrong prefix length '" << len << "' in BrownCluster specification!" << endl, false;
      else
        substrings.emplace_back(len);
    }

    clusters.clear();
    unordered_map<string, unsigned> cluster_map;
    unordered_map<string, ner_feature> prefixes_map;
    string line;
    vector<string> tokens;
    while (getline(in, line)) {
      split(line, '\t', tokens);
      if (tokens.size() != 2) return cerr << "Wrong line '" << line << "' in Brown cluster file '" << args[0] << "'!" << endl, false;

      string cluster = tokens[0], form = tokens[1];
      auto it = cluster_map.find(cluster);
      if (it == cluster_map.end()) {
        unsigned id = clusters.size();
        clusters.emplace_back();
        for (auto&& substring : substrings)
          if (substring == string::npos || substring < cluster.size())
            clusters.back().emplace_back(prefixes_map.emplace(cluster.substr(0, substring), *total_features + (2*window + 1) * prefixes_map.size() + window).first->second);
        it = cluster_map.emplace(cluster, id).first;
      }
      if (!map.emplace(form, it->second).second) return cerr << "Form '" << form << "' is present twice in Brown cluster file '" << args[0] << "'!" << endl, false;
    }

    *total_features += (2*window + 1) * prefixes_map.size();
    return true;
  }

  virtual void load(binary_decoder& data, const nlp_pipeline& pipeline) override {
    feature_processor::load(data, pipeline);

    clusters.resize(data.next_4B());
    for (auto&& cluster : clusters) {
      cluster.resize(data.next_4B());
      for (auto&& feature : cluster)
        feature = data.next_4B();
    }
  }

  virtual void save(binary_encoder& enc) override {
    feature_processor::save(enc);

    enc.add_4B(clusters.size());
    for (auto&& cluster : clusters) {
      enc.add_4B(cluster.size());
      for (auto&& feature : cluster)
        enc.add_4B(feature);
    }
  }

  virtual void process_sentence(ner_sentence& sentence, ner_feature* /*total_features*/, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++) {
      auto it = map.find(sentence.words[i].raw_lemma);
      if (it != map.end()) {
        auto& cluster = clusters[it->second];
        for (auto&& feature : cluster)
          apply_in_window(i, feature);
      }
    }
  }

 private:
  vector<vector<ner_feature>> clusters;
};


// CzechAddContainers
class czech_add_containers : public feature_processor {
 public:
  virtual bool parse(int window, const vector<string>& args, entity_map& entities, ner_feature* total_features, const nlp_pipeline& pipeline) {
    if (window) return cerr << "CzechAddContainers cannot have non-zero window!" << endl, false;

    return feature_processor::parse(window, args, entities, total_features, pipeline);
  }

  virtual void process_entities(ner_sentence& /*sentence*/, vector<named_entity>& entities, vector<named_entity>& buffer) const override {
    buffer.clear();

    for (unsigned i = 0; i < entities.size(); i++) {
      // P if ps+ pf+
      if (entities[i].type.compare("pf") == 0 && (!i || entities[i-1].start + entities[i-1].length < entities[i].start || entities[i-1].type.compare("pf") != 0)) {
        unsigned j = i + 1;
        while (j < entities.size() && entities[j].start == entities[j-1].start + entities[j-1].length && entities[j].type.compare("pf") == 0) j++;
        if (j < entities.size() && entities[j].start == entities[j-1].start + entities[j-1].length && entities[j].type.compare("ps") == 0) {
          j++;
          while (j < entities.size() && entities[j].start == entities[j-1].start + entities[j-1].length && entities[j].type.compare("ps") == 0) j++;
          buffer.emplace_back(entities[i].start, entities[j - 1].start + entities[j - 1].length - entities[i].start, "P");
        }
      }

      // T if td tm ty | td tm
      if (entities[i].type.compare("td") == 0 && i+1 < entities.size() && entities[i+1].start == entities[i].start + entities[i].length && entities[i+1].type.compare("tm") == 0) {
        unsigned j = i + 2;
        if (j < entities.size() && entities[j].start == entities[j-1].start + entities[j-1].length && entities[j].type.compare("ty") == 0) j++;
        buffer.emplace_back(entities[i].start, entities[j - 1].start + entities[j - 1].length - entities[i].start, "T");
      }
      // T if !td tm ty
      if (entities[i].type.compare("tm") == 0 && (!i || entities[i-1].start + entities[i-1].length < entities[i].start || entities[i-1].type.compare("td") != 0))
        if (i+1 < entities.size() && entities[i+1].start == entities[i].start + entities[i].length && entities[i+1].type.compare("ty") == 0)
          buffer.emplace_back(entities[i].start, entities[i + 1].start + entities[i + 1].length - entities[i].start, "T");

      buffer.push_back(entities[i]);
    }

    if (buffer.size() > entities.size()) entities = buffer;
  }

  // CzechAddContainers used to be entity_processor which had empty load and save methods.
  virtual void load(binary_decoder& /*data*/, const nlp_pipeline& /*pipeline*/) override {}
  virtual void save(binary_encoder& /*enc*/) override {}
};


// CzechLemmaTerm
class czech_lemma_term : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& buffer) const override {
    for (unsigned i = 0; i < sentence.size; i++) {
      for (unsigned pos = 0; pos + 2 < sentence.words[i].lemma_comments.size(); pos++)
        if (sentence.words[i].lemma_comments[pos] == '_' && sentence.words[i].lemma_comments[pos+1] == ';') {
          buffer.assign(1, sentence.words[i].lemma_comments[pos+2]);
          apply_in_window(i, lookup(buffer, total_features));
        }
    }
  }
};


// Form
class form : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++)
      apply_in_window(i, lookup(sentence.words[i].form, total_features));

    apply_outer_words_in_window(lookup_empty());
  }
};


// FormCapitalization
class form_capitalization : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& buffer) const override {
    using namespace unilib;

    ner_feature fst_cap = lookup(buffer.assign("f"), total_features);
    ner_feature all_cap = lookup(buffer.assign("a"), total_features);
    ner_feature mixed_cap = lookup(buffer.assign("m"), total_features);

    for (unsigned i = 0; i < sentence.size; i++) {
      bool was_upper = false, was_lower = false;

      auto* form = sentence.words[i].form.c_str();
      char32_t chr;
      for (bool first = true; (chr = utf8::decode(form)); first = false) {
        auto category = unicode::category(chr);
        was_upper = was_upper || category & unicode::Lut;
        was_lower = was_lower || category & unicode::Ll;

        if (first && was_upper) apply_in_window(i, fst_cap);
      }
      if (was_upper && !was_lower) apply_in_window(i, all_cap);
      if (was_upper && was_lower) apply_in_window(i, mixed_cap);
    }
  }
};


// Gazetteers
class gazetteers : public feature_processor {
 public:
  enum { G = 0, U = 1, B = 2, L = 3, I = 4 };

  virtual bool parse(int window, const vector<string>& args, entity_map& entities,
                     ner_feature* total_features, const nlp_pipeline& pipeline) override {
    if (!feature_processor::parse(window, args, entities, total_features, pipeline)) return false;

    gazetteers_info.clear();
    for (auto&& arg : args) {
      ifstream in(arg.c_str());
      if (!in.is_open()) return cerr << "Cannot open gazetteers file '" << arg << "'!" << endl, false;

      unsigned longest = 0;
      string gazetteer;
      string line;
      vector<string> tokens;
      while (getline(in, line)) {
        split(line, ' ', tokens);
        for (unsigned i = 0; i < tokens.size(); i++)
          if (!tokens[i][0])
            tokens.erase(tokens.begin() + i--);
        if (tokens.size() > longest) longest = tokens.size();

        gazetteer.clear();
        for (unsigned i = 0; i < tokens.size(); i++) {
          if (i) gazetteer += ' ';
          gazetteer += tokens[i];
          auto it = map.emplace(gazetteer, gazetteers_info.size()).first;
          if (it->second == gazetteers_info.size()) gazetteers_info.emplace_back();
          auto& info = gazetteers_info[it->second];
          if (i + 1 < tokens.size())
            info.prefix_of_longer |= true;
          else
            if (find(info.features.begin(), info.features.end(), *total_features + window) == info.features.end())
              info.features.emplace_back(*total_features + window);
        }
      }
      *total_features += (2*window + 1) * (longest == 0 ? 0 : longest == 1 ? U+1 : longest == 2 ? L+1 : I+1);
    }

    return true;
  }

  virtual void load(binary_decoder& data, const nlp_pipeline& pipeline) override {
    feature_processor::load(data, pipeline);

    gazetteers_info.resize(data.next_4B());
    for (auto&& gazetteer : gazetteers_info) {
      gazetteer.prefix_of_longer = data.next_1B();
      gazetteer.features.resize(data.next_1B());
      for (auto&& feature : gazetteer.features)
        feature = data.next_4B();
    }
  }

  virtual void save(binary_encoder& enc) override {
    feature_processor::save(enc);

    enc.add_4B(gazetteers_info.size());
    for (auto&& gazetteer : gazetteers_info) {
      enc.add_1B(gazetteer.prefix_of_longer);
      enc.add_1B(gazetteer.features.size());
      for (auto&& feature : gazetteer.features)
        enc.add_4B(feature);
    }
  }

  virtual void process_sentence(ner_sentence& sentence, ner_feature* /*total_features*/, string& buffer) const override {
    for (unsigned i = 0; i < sentence.size; i++) {
      auto it = map.find(sentence.words[i].raw_lemma);
      if (it == map.end()) continue;

      // Apply regular gazetteer feature G + unigram gazetteer feature U
      for (auto&& feature : gazetteers_info[it->second].features) {
        apply_in_window(i, feature + G * (2*window + 1));
        apply_in_window(i, feature + U * (2*window + 1));
      }

      for (unsigned j = i + 1; gazetteers_info[it->second].prefix_of_longer && j < sentence.size; j++) {
        if (j == i + 1) buffer.assign(sentence.words[i].raw_lemma);
        buffer += ' ';
        buffer += sentence.words[j].raw_lemma;
        it = map.find(buffer);
        if (it == map.end()) break;

        // Apply regular gazetteer feature G + position specific gazetteers B, I, L
        for (auto&& feature : gazetteers_info[it->second].features)
          for (unsigned g = i; g <= j; g++) {
            apply_in_window(g, feature + G * (2*window + 1));
            apply_in_window(g, feature + (g == i ? B : g == j ? L : I) * (2*window + 1));
          }
      }
    }
  }

 private:
  struct gazetteer_info {
    vector<ner_feature> features;
    bool prefix_of_longer;
  };
  vector<gazetteer_info> gazetteers_info;
};


// Lemma
class lemma : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++)
      apply_in_window(i, lookup(sentence.words[i].lemma_id, total_features));

    apply_outer_words_in_window(lookup_empty());
  }
};


// NumericTimeValue
class number_time_value : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& buffer) const override {
    ner_feature hour = lookup(buffer.assign("H"), total_features);
    ner_feature minute = lookup(buffer.assign("M"), total_features);
    ner_feature time = lookup(buffer.assign("t"), total_features);
    ner_feature day = lookup(buffer.assign("d"), total_features);
    ner_feature month = lookup(buffer.assign("m"), total_features);
    ner_feature year = lookup(buffer.assign("y"), total_features);

    for (unsigned i = 0; i < sentence.size; i++) {
      const char* form = sentence.words[i].form.c_str();
      unsigned num;
      bool digit;

      for (digit = false, num = 0; *form; form++) {
        if (*form < '0' || *form > '9') break;
        digit = true;
        num = num * 10 + *form - '0';
      }
      if (digit && !*form) {
        // We have a number
        if (num < 24) apply_in_window(i, hour);
        if (num < 60) apply_in_window(i, minute);
        if (num >= 1 && num <= 31) apply_in_window(i, day);
        if (num >= 1 && num <= 12) apply_in_window(i, month);
        if (num >= 1000 && num <= 2200) apply_in_window(i, year);;
      }
      if (digit && num < 24 && (*form == '.' || *form == ':')) {
        // Maybe time
        for (digit = false, num = 0, form++; *form; form++) {
          if (*form < '0' || *form > '9') break;
          digit = true;
          num = num * 10 + *form - '0';
        }
        if (digit && !*form && num < 60) apply_in_window(i, time);
      }
    }
  }
};


// PreviousStage
class previous_stage : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& buffer) const override {
    for (unsigned i = 0; i < sentence.size; i++)
      if (sentence.previous_stage[i].bilou != bilou_type_unknown) {
        buffer.clear();
        append_encoded(buffer, sentence.previous_stage[i].bilou);
        buffer.push_back(' ');
        append_encoded(buffer, sentence.previous_stage[i].entity);
        apply_in_range(i, lookup(buffer, total_features), 1, window);
      }
  }

 private:
  static void append_encoded(string& str, int value) {
    if (value < 0) {
      str.push_back('-');
      value = -value;
    }
    for (; value; value >>= 4)
      str.push_back("0123456789abcdef"[value & 0xF]);
  }
};


// RawLemma
class raw_lemma : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++)
      apply_in_window(i, lookup(sentence.words[i].raw_lemma, total_features));

    apply_outer_words_in_window(lookup_empty());
  }
};


// RawLemmaCapitalization
class raw_lemma_capitalization : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& buffer) const override {
    using namespace unilib;

    ner_feature fst_cap = lookup(buffer.assign("f"), total_features);
    ner_feature all_cap = lookup(buffer.assign("a"), total_features);
    ner_feature mixed_cap = lookup(buffer.assign("m"), total_features);

    for (unsigned i = 0; i < sentence.size; i++) {
      bool was_upper = false, was_lower = false;

      auto* raw_lemma = sentence.words[i].raw_lemma.c_str();
      char32_t chr;
      for (bool first = true; (chr = utf8::decode(raw_lemma)); first = false) {
        auto category = unicode::category(chr);
        was_upper = was_upper || category & unicode::Lut;
        was_lower = was_lower || category & unicode::Ll;

        if (first && was_upper) apply_in_window(i, fst_cap);
      }
      if (was_upper && !was_lower) apply_in_window(i, all_cap);
      if (was_upper && was_lower) apply_in_window(i, mixed_cap);
    }
  }
};


// Tag
class tag : public feature_processor {
 public:
  virtual void process_sentence(ner_sentence& sentence, ner_feature* total_features, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++)
      apply_in_window(i, lookup(sentence.words[i].tag, total_features));

    apply_outer_words_in_window(lookup_empty());
  }
};


// URLEmailDetector
class url_email_detector : public feature_processor {
 public:
  virtual bool parse(int window, const vector<string>& args, entity_map& entities,
                     ner_feature* total_features, const nlp_pipeline& pipeline) override {
    if (!feature_processor::parse(window, args, entities, total_features, pipeline)) return false;
    if (args.size() != 2) return cerr << "URLEmailDetector requires exactly two arguments -- named entity types for URL and email!" << endl, false;

    url = entities.parse(args[0].c_str(), true);
    email = entities.parse(args[1].c_str(), true);

    if (url == entity_type_unknown || email == entity_type_unknown)
      return cerr << "Cannot create entities '" << args[0] << "' and '" << args[1] << "' in URLEmailDetector!" << endl, false;
    return true;
  }

  virtual void load(binary_decoder& data, const nlp_pipeline& pipeline) override {
    feature_processor::load(data, pipeline);

    url = data.next_4B();
    email = data.next_4B();
  }

  virtual void save(binary_encoder& enc) override {
    feature_processor::save(enc);

    enc.add_4B(url);
    enc.add_4B(email);
  }

  virtual void process_sentence(ner_sentence& sentence, ner_feature* /*total_features*/, string& /*buffer*/) const override {
    for (unsigned i = 0; i < sentence.size; i++) {
      auto type = url_detector::detect(sentence.words[i].form);
      if (type == url_detector::NO_URL || sentence.probabilities[i].local_filled) continue;

      // We have found URL or email and the word has not yet been determined
      for (auto&& bilou : sentence.probabilities[i].local.bilou) {
        bilou.probability = 0.;
        bilou.entity = entity_type_unknown;
      }
      sentence.probabilities[i].local.bilou[bilou_type_U].probability = 1.;
      sentence.probabilities[i].local.bilou[bilou_type_U].entity = type == url_detector::EMAIL ? email : url;
      sentence.probabilities[i].local_filled = true;
    }
  }

 private:
  entity_type url, email;
};


} // namespace feature_processors

// Feature processor factory method
feature_processor* feature_processor::create(const string& name) {
  if (name.compare("BrownClusters") == 0) return new feature_processors::brown_clusters();
  if (name.compare("CzechAddContainers") == 0) return new feature_processors::czech_add_containers();
  if (name.compare("CzechLemmaTerm") == 0) return new feature_processors::czech_lemma_term();
  if (name.compare("Form") == 0) return new feature_processors::form();
  if (name.compare("FormCapitalization") == 0) return new feature_processors::form_capitalization();
  if (name.compare("Gazetteers") == 0) return new feature_processors::gazetteers();
  if (name.compare("Lemma") == 0) return new feature_processors::lemma();
  if (name.compare("NumericTimeValue") == 0) return new feature_processors::number_time_value();
  if (name.compare("PreviousStage") == 0) return new feature_processors::previous_stage();
  if (name.compare("RawLemma") == 0) return new feature_processors::raw_lemma();
  if (name.compare("RawLemmaCapitalization") == 0) return new feature_processors::raw_lemma_capitalization();
  if (name.compare("Tag") == 0) return new feature_processors::tag();
  if (name.compare("URLEmailDetector") == 0) return new feature_processors::url_email_detector();
  return nullptr;
}

} // namespace nametag
} // namespace ufal