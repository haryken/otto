#ifndef OTTO_WEB_CONTROL_H
#define OTTO_WEB_CONTROL_H

#include <string>

/**
 * Run a movement from Self-Control web (:8080).
 * action: forward | backward | left | right | swing | sit | home | stop |
 *         explore_on | explore_off | detach | attach | hide_qr | stop_web |
 *         jump | moonwalk_l | moonwalk_r | bend_l | bend_r |
 *         shake_l | shake_r | updown | tiptoe | jitter | ascending |
 *         crusaito | flapping | whirlwind | showcase
 * @return empty string on success, otherwise an error message.
 */
std::string OttoWebControlAction(const std::string& action);

/**
 * JSON pose snapshot for walk-finish calibration.
 * angles are last commanded software positions (servos have no feedback).
 */
std::string OttoWebControlGetPoseJson();

/**
 * Detach one joint (ll/rl/lf/rf/lh/rh) so it can be hand-rotated.
 */
std::string OttoWebControlDetachJoint(const std::string& joint);

/**
 * Set one joint angle (0-180) and hold it (re-attaches that servo).
 */
std::string OttoWebControlSetJoint(const std::string& joint, int angle);

/**
 * Servo nest trim (offset from commanded 90°). Persisted in NVS otto_trims.
 */
std::string OttoWebControlGetTrimsJson();

/**
 * Apply trims live; if save=true write NVS; if preview=true soft-Home to nest.
 * Values clamped to [-50, 50]. Keys: left_leg, right_leg, left_foot, right_foot,
 * left_hand, right_hand (missing keys keep current NVS value).
 */
std::string OttoWebControlApplyTrimsJson(const std::string& body_json);

bool OttoWebGetMotorsOnDemand();
void OttoWebSetMotorsOnDemand(bool on_demand);

/** Start LAN Self-Control HTTP :8080 (on-demand). True if listening. */
bool OttoSelfControlWebStart();
/** Stop :8080 and release sockets / WiFi PS hold. */
void OttoSelfControlWebStop();
bool OttoSelfControlWebIsRunning();

#endif
