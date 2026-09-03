#ifndef OTTO_MOTOR_TEST_H
#define OTTO_MOTOR_TEST_H

// Called from WiFi config portal (Save / checkbox). Not MCP.
// enable=true: walk forward continuously (same ACTION_WALK path as self control).
// enable=false: stop and return to home pose.
void OttoWifiConfigMotorTestForward(bool enable);

#endif
