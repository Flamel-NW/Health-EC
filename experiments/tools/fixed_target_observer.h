#pragma once

// Passive, frozen-issued-pattern single-substitution diagnostics. No policy API.
#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <vector>

namespace healthec::diagnostic {

struct FixedTargetPast {
    int shard = 0, source = 0, target = -1, target_chosen_read = 0;
    long reads = 0, available = 0;
    double gain = 0;
};

struct FixedTargetDecision {
    int past_window = 0, future_window = 0, budget = 0, decision_read = 0;
    FixedTargetPast past;
    long future_reads = 0, available_reads = 0, source_changed = 0, target_conflict = 0;
    long negative_reads = 0, zero_reads = 0;
    double gain = 0;
    std::array<long, 2> tail_removed{}, tail_added{};
};

class FixedTargetObserver {
    int window_ = -1;
    std::map<std::pair<int,int>, FixedTargetPast> past_;
    std::map<int, std::vector<std::size_t>> active_;
public:
    std::vector<FixedTargetDecision> decisions;

    template<class Layout>
    void begin_window(int window, int decision_read, const Layout& layout) {
        if (window == window_) return;
        active_.clear();
        std::map<int,int> source_counts;
        for (const auto& [key, p] : past_) { (void)key; source_counts[p.shard]++; }
        std::vector<FixedTargetPast> ranked;
        for (const auto& [key, p] : past_) {
            (void)key;
            if (source_counts[p.shard] == 1 && p.target >= 0 && p.reads > 0 &&
                p.available == p.reads && p.gain > 0 && layout.at(p.shard) == p.source)
                ranked.push_back(p);
        }
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            if (a.gain != b.gain) return a.gain > b.gain;
            return std::tie(a.shard,a.source) < std::tie(b.shard,b.source);
        });
        if (window_ >= 0 && window == window_+1) {
            for (int budget : {1,4,16}) {
                for (int i = 0; i < std::min(budget, static_cast<int>(ranked.size())); ++i) {
                    active_[ranked[i].shard].push_back(decisions.size());
                    FixedTargetDecision d;
                    d.past_window=window_; d.future_window=window; d.budget=budget;
                    d.decision_read=decision_read; d.past=ranked[i];
                    decisions.push_back(d);
                }
            }
        }
        past_.clear();
        window_=window;
    }

    template<class Choose, class Legal, class Completion>
    void observe(int shard, int source, int read, double original,
                 Choose choose, Legal legal, Completion alternative) {
        const auto key=std::make_pair(shard,source);
        auto [it, inserted]=past_.try_emplace(key);
        auto& p=it->second;
        if (inserted) {
            p.shard=shard; p.source=source; p.target=choose(); p.target_chosen_read=read;
        }
        p.reads++;
        if (p.target >= 0 && legal(p.target)) {
            p.available++; p.gain += original-alternative(p.target);
        }
        const auto active=active_.find(shard);
        if (active == active_.end()) return;
        for (auto index : active->second) {
            auto& d=decisions[index];
            d.future_reads++;
            if (source != d.past.source) { d.source_changed++; continue; }
            if (!legal(d.past.target)) { d.target_conflict++; continue; }
            const double changed=alternative(d.past.target);
            const double gain=original-changed;
            d.available_reads++; d.gain += gain;
            d.negative_reads += gain < 0; d.zero_reads += gain == 0;
            for (int i=0; i<2; ++i) {
                const double threshold=i==0 ? 15. : 30.;
                d.tail_removed[i] += original>threshold && changed<=threshold;
                d.tail_added[i] += original<=threshold && changed>threshold;
            }
        }
    }
};
} // namespace healthec::diagnostic
