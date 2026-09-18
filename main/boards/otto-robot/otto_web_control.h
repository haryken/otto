#ifndef OTTO_WEB_CONTROL_H
#define OTTO_WEB_CONTROL_H

#include <string>

/**
 * Run a movement from Self-Control web (:8080).
 * action: forward | backward | left | right | jump | swing | sit | home | stop
 * @return empty string on success, otherwise an error message.
 */
std::string OttoWebControlAction(const std::string& action);

#endif
