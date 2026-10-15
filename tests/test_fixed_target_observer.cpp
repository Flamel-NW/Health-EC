#include "../experiments/tools/fixed_target_observer.h"
#include <stdexcept>
#include <unordered_map>

static void require(bool condition) {
    if (!condition) throw std::runtime_error("fixed target observer contract failed");
}

int main() {
    using healthec::diagnostic::FixedTargetObserver;
    FixedTargetObserver o;
    std::unordered_map<int,int> layout{{1,1},{2,2},{3,3}};
    o.begin_window(0,0,layout);
    auto legal=[](int target){return target==9;};
    o.observe(1,1,2,20.,[]{return 9;},legal,[](int){return 10.;});
    // Future target chooser cannot replace the previously frozen target.
    o.begin_window(1,100,layout);
    o.observe(1,1,102,10.,[]{return 8;},legal,[](int target){require(target==9);return 30.;});
    require(o.decisions.size()==3);
    for (const auto& d:o.decisions) {
        require(d.past.target==9 && d.past.target_chosen_read==2 && d.decision_read==100);
        require(d.future_reads==1 && d.available_reads==1 && d.gain==-20 && d.negative_reads==1);
    }
    // Source movement and legal-target loss are missing, never zero-benefit.
    o.observe(1,2,103,10.,[]{return -1;},legal,[](int){return 0.;});
    o.observe(1,1,104,10.,[]{return 9;},[](int){return false;},[](int){return 0.;});
    for (const auto& d:o.decisions) require(d.source_changed==1 && d.target_conflict==1 && d.available_reads==1);
    FixedTargetObserver missing;
    missing.begin_window(0,0,layout);
    missing.observe(2,2,5,20.,[]{return 9;},legal,[](int){return 10.;});
    missing.begin_window(1,100,layout);
    missing.observe(3,3,105,10.,[]{return 9;},legal,[](int){return 10.;});
    for (const auto& d:missing.decisions) require(d.future_reads==0 && d.available_reads==0);
    // Decision-time source movement excludes the past candidate.
    FixedTargetObserver moved;
    moved.begin_window(0,0,layout);
    moved.observe(1,1,4,20.,[]{return 9;},legal,[](int){return 10.;});
    layout[1]=2;
    moved.begin_window(1,100,layout);
    require(moved.decisions.empty());
}
