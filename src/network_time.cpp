#include "campello_net/network_time.hpp"

#include <algorithm>
#include <cmath>

namespace systems::leal::campello_net {

void NetworkTime::record_sample(double t0, double t1, double t2, double t3) noexcept {
    double raw_rtt = (t3 - t0) - (t2 - t1);
    double raw_offset = ((t1 - t0) + (t2 - t3)) * 0.5;

    if (sample_count_ == 0) {
        smoothed_offset_ = raw_offset;
        smoothed_rtt_ = raw_rtt;
    } else {
        smoothed_offset_ += ALPHA_OFFSET * (raw_offset - smoothed_offset_);
        smoothed_rtt_ += ALPHA_RTT * (raw_rtt - smoothed_rtt_);
    }

    ++sample_count_;
}

double NetworkTime::local_to_remote(double local_time) const noexcept {
    return local_time + smoothed_offset_;
}

double NetworkTime::remote_to_local(double remote_time) const noexcept {
    return remote_time - smoothed_offset_;
}

void NetworkTime::reset() noexcept {
    smoothed_offset_ = 0.0;
    smoothed_rtt_ = 0.0;
    sample_count_ = 0;
}

} // namespace systems::leal::campello_net
