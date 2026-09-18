#ifndef WEBSOCKET_CONTROL_SERVER_H
#define WEBSOCKET_CONTROL_SERVER_H

#include <esp_http_server.h>
#include <cJSON.h>
#include <string>
#include <map>

class WebSocketControlServer {
public:
    WebSocketControlServer();
    ~WebSocketControlServer();

    bool Start(int port = 8080);
    
    void Stop();

    bool IsRunning() const { return server_handle_ != nullptr; }

    size_t GetClientCount() const;

    // Self-control: read student info from NVS
    static std::string GetStudentName();
    static int GetPresetMacIdx();
    static std::string GetUnitsForCourse(int course_idx);
    static int GetYoungInnovatorsSubIdx();
    static std::string GetYoungInnovatorsUnit(int sub_idx);
    static int GetExplorersSubIdx();
    static std::string GetExplorersUnit(int sub_idx);
    static int GetFutureLeadersSubIdx();
    static std::string GetFutureLeadersUnit(int sub_idx);
    /** Sub-book index for courses that have subs; 0 otherwise. */
    static int GetActiveSubIdx(int course_idx);
    /** Selected unit id string for the active course (+ sub if any). */
    static std::string GetActiveUnitSelection(int course_idx);
    /**
     * Move active course unit by delta (+1 next, -1 previous), save NVS.
     * Returns JSON: success, unit_index, unit_name, message (or error).
     */
    static std::string ShiftActiveUnit(int delta);

private:
    httpd_handle_t server_handle_;
    std::map<int, httpd_req_t*> clients_;

    static esp_err_t ws_handler(httpd_req_t *req);
    static esp_err_t self_control_page_handler(httpd_req_t *req);
    static esp_err_t trim_guide_handler(httpd_req_t *req);
    static esp_err_t api_config_get_handler(httpd_req_t *req);
    static esp_err_t api_config_post_handler(httpd_req_t *req);
    static esp_err_t api_robot_get_handler(httpd_req_t *req);
    static esp_err_t api_robot_post_handler(httpd_req_t *req);
    static esp_err_t api_ota_get_handler(httpd_req_t *req);
    static esp_err_t api_ota_post_handler(httpd_req_t *req);
    static esp_err_t api_ota_upload_handler(httpd_req_t *req);
    static esp_err_t api_action_post_handler(httpd_req_t *req);
    static esp_err_t api_unit_post_handler(httpd_req_t *req);
    static esp_err_t api_pose_get_handler(httpd_req_t *req);
    static esp_err_t api_pose_post_handler(httpd_req_t *req);
    static esp_err_t api_trim_get_handler(httpd_req_t *req);
    static esp_err_t api_trim_post_handler(httpd_req_t *req);
    
    void HandleMessage(httpd_req_t *req, const char* data, size_t len);
    void AddClient(httpd_req_t *req);
    void RemoveClient(httpd_req_t *req);
    static WebSocketControlServer* instance_;
};

#endif // WEBSOCKET_CONTROL_SERVER_H

