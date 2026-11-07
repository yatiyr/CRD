// HESAP-NET's dead-reckoning peer, behind plain calls: KDIS's DeadReckoningCalculator (dead_reckoning_peers.cpp), the
// one C++ implementation of IEEE 1278.1's algorithms a maintained DIS library carries.

#pragma once

#include <cstddef>
#include <memory>

namespace dead_reckoning_peers
{

// An entity's last update, in KDIS's single-precision fields, with DIS's Euler angles in radians. `algorithm` is the
// standard's number, 2 to 9.
struct Update
{
    double position[3];
    float velocity[3];
    float acceleration[3];
    float rate[3];
    float psi;
    float theta;
    float phi;
    int algorithm;
};

// One DeadReckoningCalculator per entity, as a client holds them: Reset on an update, RunAlgorithm on each frame.
class KdisFleet
{
public:
    explicit KdisFleet(std::size_t count);
    ~KdisFleet();
    KdisFleet(const KdisFleet&) = delete;
    KdisFleet& operator=(const KdisFleet&) = delete;

    void reset(std::size_t entity, const Update& update);

    // Each entity's position at its own time since its update.
    void run(const float* dt, std::size_t count, double* x, double* y, double* z);

private:
    struct Calculators;
    std::unique_ptr<Calculators> m_calculators;
};

} // namespace dead_reckoning_peers
