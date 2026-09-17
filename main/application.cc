#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "device_identity_presets.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    // Pool courses (Explorers / daily-chat): pick Device-Id from MAC pool every boot.
    const uint8_t boot_course = ReadPresetMacIndexFromNvs();
    if (CourseUsesMacPool(boot_course)) {
        const char* mac = PickAndSaveMacPoolForCourse(boot_course);
        ESP_LOGI(TAG, "Boot MAC pool (course %u): %s", (unsigned)boot_course, mac ? mac : "(none)");
    }

    // Fresh Client-Id every boot (with current Device-Id) to avoid server spam/timeout.
    auto& board = Board::GetInstance();
    board.RegenerateUuid();

    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // WiFi status toasts on the status bar (must stay short — long timers blocked the clock).
    constexpr int kWifiStatusToastMs = 2000;

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this, kWifiStatusToastMs](NetworkEvent event, const std::string& data) {
        switch (event) {
            case NetworkEvent::Scanning:
                Schedule([kWifiStatusToastMs]() {
                    Board::GetInstance().GetDisplay()->ShowNotification(
                        Lang::Strings::SCANNING_WIFI, kWifiStatusToastMs);
                });
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    Schedule([]() {
                        Board::GetInstance().GetDisplay()->SetStatus(
                            Lang::Strings::REGISTERING_NETWORK);
                    });
                }
                // No long "Kết nối đến..." overlay (was 30s and overlapped "Đã kết nối").
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                Schedule([msg = std::move(msg), kWifiStatusToastMs]() {
                    Board::GetInstance().GetDisplay()->ShowNotification(
                        msg.c_str(), kWifiStatusToastMs);
                });
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                Schedule([]() {
                    Board::GetInstance().GetDisplay()->DismissNotification();
                });
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                Schedule([]() {
                    Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::DETECTING_MODULE);
                });
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                Schedule([]() {
                    Board::GetInstance().GetDisplay()->SetStatus(
                        Lang::Strings::REGISTERING_NETWORK);
                });
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }

#if CONFIG_SILENCE_PROMPT_ENABLE
            CheckSilencePrompt();
#endif
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    Schedule([]() {
        Board::GetInstance().GetDisplay()->UpdateStatusBar(true);
    });
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        PrepareAudioChannelClose();
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    ESP_LOGI(TAG, "Firmware %s%s", Lang::Strings::VERSION, ota_->GetCurrentVersion().c_str());
    Board::GetInstance().GetDisplay()->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol(bool prefer_websocket) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    Settings ws_settings("websocket", false);
    Settings mqtt_settings("mqtt", false);
    const bool has_ws = !ws_settings.GetString("url").empty() ||
                        (ota_ && ota_->HasWebsocketConfig());
    const bool has_mqtt = !mqtt_settings.GetString("endpoint").empty() ||
                          (ota_ && ota_->HasMqttConfig());

    if (prefer_websocket && has_ws) {
        // Course switch: Device-Id is a per-connection WebSocket header.
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else if (ota_ && ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_ && ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else if (has_mqtt) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (has_ws) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (audio_service_.IsLocalPlaybackActive()) {
            return;
        }
        if (mute_url_tts_.load()) {
            return;
        }
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        PrepareAudioChannelClose();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            if (suppress_channel_closed_idle_) {
                ESP_LOGI(TAG, "Skip idle after channel close (course switch in progress)");
                return;
            }
            if (audio_service_.IsLocalPlaybackActive()) {
                ESP_LOGI(TAG, "Audio channel closed for music-only mode (playback continues)");
                return;
            }
            audio_service_.StopLocalPlaybackIfActive();
            audio_service_.RestoreAudioHardwareAfterLocalPlayback();
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
#if CONFIG_SILENCE_PROMPT_ENABLE
            NotifyServerSpeechActivity();
#endif
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                mute_url_tts_.store(false);
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                mute_url_tts_.store(false);
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    // Giống Android Mini: không đọc URL / IP / :8080 khi hiện QR Self-Control.
                    const char* t = text->valuestring;
                    const bool looks_like_url =
                        strstr(t, "http://") != nullptr || strstr(t, "https://") != nullptr ||
                        strstr(t, ":8080") != nullptr ||
                        (strstr(t, "192.168.") != nullptr && strstr(t, "self.otto") == nullptr);
                    mute_url_tts_.store(looks_like_url);
                    if (looks_like_url) {
                        ESP_LOGW(TAG, "Mute TTS (URL/IP leak): %s", t);
                    }
                    const bool music_play =
                        strstr(text->valuestring, "self.otto.music.play") != nullptr;
                    Schedule([this, display, message = std::string(text->valuestring), music_play, looks_like_url]() {
                        if (music_play) {
                            audio_service_.SetLocalPlaybackActive(true);
                            audio_service_.SetCaptureSuspended(true);
                            EnterMusicOnlyModeImpl();
                        } else if (!audio_service_.IsLocalPlaybackActive() && !looks_like_url) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
#if CONFIG_SILENCE_PROMPT_ENABLE
            NotifyServerSpeechActivity();
#endif
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    if (!audio_service_.IsLocalPlaybackActive()) {
                        display->SetChatMessage("user", message.c_str());
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    if (!audio_service_.IsLocalPlaybackActive()) {
                        display->SetChatMessage("system", payload_str.c_str());
                    }
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (audio_service_.IsLocalPlaybackActive()) {
        audio_service_.StopLocalPlaybackIfActive();
        return;
    }

    if (state == kDeviceStateIdle) {
        audio_service_.StopLocalPlaybackIfActive();
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        protocol_->CloseAudioChannel();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    audio_service_.StopLocalPlaybackIfActive();

    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    const bool was_playing_music = audio_service_.IsLocalPlaybackActive();
    audio_service_.StopLocalPlaybackIfActive();
    if (was_playing_music) {
        ESP_LOGI(TAG, "Wake word stopped music playback");
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        if (state == kDeviceStateSpeaking) {
            AbortSpeaking(kAbortReasonWakeWordDetected);
        }
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            if (protocol_->IsAudioChannelOpened()) {
                protocol_->SendStartListening(listening_mode_);
            }
            audio_service_.ResetDecoder();
            if (protocol_->IsAudioChannelOpened()) {
                audio_service_.PlayNotificationSound(Lang::Sounds::OGG_POPUP);
            }
            audio_service_.EnableWakeWordDetection(true);
        } else {
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // Encode and send the wake word data to the server
    while (auto packet = audio_service_.PopWakeWordPacket()) {
        protocol_->SendAudio(std::move(packet));
    }
    // Set the chat state to wake word detected
    protocol_->SendWakeWordDetected(wake_word);
    SetListeningMode(GetDefaultListeningMode());
#else
    SetListeningMode(GetDefaultListeningMode());
#endif
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    
    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->DismissNotification();
            display->SetStatus(Lang::Strings::STANDBY);
            if (!music_playing_display_active_) {
                display->ClearChatMessages();
            }
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            if (!audio_service_.IsLocalPlaybackActive()) {
                audio_service_.StopLocalPlaybackIfActive();
                audio_service_.RestoreAudioHardwareAfterLocalPlayback();
            }
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

#if CONFIG_SILENCE_PROMPT_ENABLE
            ResetSilencePromptTracking();
#endif

            if (!audio_service_.IsLocalPlaybackActive()) {
                audio_service_.RestoreAudioHardwareAfterLocalPlayback();
            }

            if (!audio_service_.IsCaptureSuspended() &&
                !audio_service_.IsAudioProcessorRunning()) {
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                if (protocol_) {
                    protocol_->SendStartListening(listening_mode_);
                }
                audio_service_.EnableVoiceProcessing(true);
            }

            // Otto: tút khi icon micro (trừ goodbye/ngắt kết nối). Mic đã bật ở trên (giống v3).
            if (!suppress_listening_chime_ && !audio_service_.IsLocalPlaybackActive() && protocol_) {
                audio_service_.PlayNotificationSound(Lang::Sounds::OGG_POPUP);
            }
            suppress_listening_chime_ = false;

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            audio_service_.EnableWakeWordDetection(false);
#endif
            break;
        }
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::EnterMusicOnlyModeImpl() {
    if (music_only_mode_entered_) {
        return;
    }
    music_only_mode_entered_ = true;

    aborted_ = true;
    if (protocol_) {
        AbortSpeaking(kAbortReasonNone);
        const auto state = GetDeviceState();
        if (state == kDeviceStateListening && protocol_->IsAudioChannelOpened()) {
            protocol_->SendStopListening();
        }
        if (protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    }
    audio_service_.EnableVoiceProcessing(false);
    const auto state = GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateStarting && state != kDeviceStateWifiConfiguring &&
        state != kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
    audio_service_.EnableWakeWordDetection(true);
    ShowMusicPlayingOnDisplayImpl();
    ESP_LOGI(TAG, "Music-only mode: standby, wake word or BOOT to stop");
}

void Application::ShowMusicPlayingOnDisplayImpl() {
    if (music_playing_display_active_) {
        return;
    }
    auto display = Board::GetInstance().GetDisplay();
    display->ClearChatMessages();
    display->SetChatMessage("system", "Đang phát nhạc");
    music_playing_display_active_ = true;
}

void Application::ShowMusicPlayingOnDisplay() {
    Schedule([this]() { ShowMusicPlayingOnDisplayImpl(); });
}

void Application::ClearMusicPlayingOnDisplay() {
    Schedule([this]() {
        if (!music_playing_display_active_) {
            return;
        }
        Board::GetInstance().GetDisplay()->ClearChatMessages();
        music_playing_display_active_ = false;
    });
}

void Application::EnterMusicOnlyMode() {
    Schedule([this]() { EnterMusicOnlyModeImpl(); });
}

void Application::ResetMusicOnlySession() {
    music_only_mode_entered_ = false;
}

void Application::FinishListeningMicSetup() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (audio_service_.IsCaptureSuspended() || audio_service_.IsLocalPlaybackActive()) {
        return;
    }

    if (protocol_) {
        protocol_->SendStartListening(listening_mode_);
    }
    if (!audio_service_.IsAudioProcessorRunning()) {
        audio_service_.EnableVoiceProcessing(true);
    }
    if (protocol_) {
        audio_service_.PlayNotificationSound(Lang::Sounds::OGG_POPUP);
    }
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::PrepareAudioChannelClose() {
    suppress_listening_chime_ = true;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    SetWebOtaState("running", "Đang tải firmware...", "");
    SetWebOtaProgress(0, 0);

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    // Keep Self-Control HTTP :8080 running so the web UI can show progress.
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        SetWebOtaProgress(progress, speed);
        char buffer[48];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, (unsigned)(speed / 1024));
        SetWebOtaState("running", std::string(buffer), "");
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        SetWebOtaState("failed", "Cập nhật thất bại — giữ firmware cũ", "download_or_validate_failed");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateIdle);
        return false;
    } else {
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        SetWebOtaState("success", "Thành công — đang khởi động lại...", "");
        SetWebOtaProgress(100, 0);
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        Reboot();
        return true;
    }
}

void Application::SetWebOtaState(const std::string& state, const std::string& message,
                                const std::string& error) {
    std::lock_guard<std::mutex> lock(web_ota_mutex_);
    web_ota_state_ = state;
    if (!message.empty()) {
        web_ota_message_ = message;
    }
    web_ota_error_ = error;
}

void Application::SetWebOtaProgress(int progress, size_t speed) {
    std::lock_guard<std::mutex> lock(web_ota_mutex_);
    web_ota_progress_ = progress;
    web_ota_speed_ = speed;
}

bool Application::IsWebOtaBusy() const {
    std::lock_guard<std::mutex> lock(web_ota_mutex_);
    return web_ota_state_ == "running";
}

std::string Application::GetWebOtaStatusJson() {
    std::lock_guard<std::mutex> lock(web_ota_mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", web_ota_state_.c_str());
    cJSON_AddNumberToObject(root, "progress", web_ota_progress_);
    cJSON_AddNumberToObject(root, "speed_kbps", static_cast<double>(web_ota_speed_ / 1024));
    cJSON_AddStringToObject(root, "message", web_ota_message_.c_str());
    cJSON_AddStringToObject(root, "error", web_ota_error_.c_str());
    cJSON_AddStringToObject(root, "url", web_ota_url_.c_str());
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
    return out;
}

bool Application::StartWebOta(const std::string& url) {
    if (url.size() < 12 || (url.find("http://") != 0 && url.find("https://") != 0)) {
        SetWebOtaState("failed", "URL không hợp lệ", "invalid_url");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(web_ota_mutex_);
        if (web_ota_state_ == "running") {
            return false;
        }
        web_ota_url_ = url;
    }
    SetWebOtaState("running", "Đang chuẩn bị cập nhật...", "");
    SetWebOtaProgress(0, 0);
    Schedule([this, url]() {
        UpgradeFirmware(url);
    });
    return true;
}

bool Application::RunWebOtaUpload(size_t content_length,
                                  std::function<int(char* buf, size_t max_len)> reader) {
    {
        std::lock_guard<std::mutex> lock(web_ota_mutex_);
        if (web_ota_state_ == "running") {
            return false;
        }
        web_ota_url_ = "(upload)";
    }

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    SetWebOtaState("running", "Đang nhận file firmware...", "");
    SetWebOtaProgress(0, 0);

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);

    SetDeviceState(kDeviceStateUpgrading);
    display->SetChatMessage("system", "OTA upload...");
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(300));

    bool upgrade_success = Ota::UpgradeFromReader(content_length, reader,
        [this, display](int progress, size_t speed) {
            SetWebOtaProgress(progress, speed);
            char buffer[48];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, (unsigned)(speed / 1024));
            SetWebOtaState("running", std::string(buffer), "");
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Upload firmware upgrade failed");
        SetWebOtaState("failed", "Upload thất bại — giữ firmware cũ", "upload_or_validate_failed");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(2000));
        SetDeviceState(kDeviceStateIdle);
        return false;
    }

    // Caller must send HTTP response, then Reboot().
    ESP_LOGI(TAG, "Upload firmware upgrade successful (pending reboot)");
    SetWebOtaState("success", "Thành công — đang khởi động lại...", "");
    SetWebOtaProgress(100, 0);
    display->SetChatMessage("system", "Upgrade successful, rebooting...");
    return true;
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    audio_service_.StopLocalPlaybackIfActive();

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}

void Application::PatchMqttIdentityToCurrentMac() {
    // Official / gateway MQTT client_id is often: GID_xxx@@@aa_bb_cc_dd_ee_ff@@@aa_bb_cc_dd_ee_ff
    const std::string mac = SystemInfo::GetMacAddress();
    std::string safe = mac;
    for (char& c : safe) {
        if (c == ':') {
            c = '_';
        }
    }

    Settings settings("mqtt", true);
    std::string client_id = settings.GetString("client_id");
    if (!client_id.empty()) {
        auto pos = client_id.find("@@@");
        if (pos != std::string::npos) {
            std::string patched = client_id.substr(0, pos) + "@@@" + safe + "@@@" + safe;
            if (patched != client_id) {
                ESP_LOGI(TAG, "Patch MQTT client_id → %s", patched.c_str());
                settings.SetString("client_id", patched);
            }
        }
    }

    std::string username = settings.GetString("username");
    // Username is frequently the MAC (with : or _).
    if (!username.empty()) {
        const bool looks_like_mac =
            (username.size() == 17 &&
             (username.find(':') != std::string::npos || username.find('_') != std::string::npos));
        if (looks_like_mac) {
            const std::string& new_user = (username.find(':') != std::string::npos) ? mac : safe;
            if (new_user != username) {
                ESP_LOGI(TAG, "Patch MQTT username → %s", new_user.c_str());
                settings.SetString("username", new_user);
            }
        }
    }
}

void Application::ApplyDeviceIdentity() {
    // Fast path (matches other FW): close old session → open new with Device-Id from NVS.
    // No CheckVersion / activation UI. Then simulate wake word so the server greets.
    Schedule([this]() {
        if (identity_task_handle_ != nullptr || activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "ApplyDeviceIdentity: another identity/activation task is running");
            return;
        }

        const std::string mac = SystemInfo::GetMacAddress();
        ESP_LOGI(TAG, "ApplyDeviceIdentity (fast): Device-Id=%s", mac.c_str());

        aborted_ = true;
        suppress_channel_closed_idle_ = true;
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(false);
        if (protocol_) {
            AbortSpeaking(kAbortReasonNone);
            if (protocol_->IsAudioChannelOpened()) {
                PrepareAudioChannelClose();
                protocol_->CloseAudioChannel();
            }
        }
        protocol_.reset();

        auto state = GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
            state == kDeviceStateConnecting || state == kDeviceStateActivating) {
            SetDeviceState(kDeviceStateIdle);
        }

        Settings ws_settings("websocket", false);
        const bool has_ws = !ws_settings.GetString("url").empty();

        // New Client-Id on every course/MAC switch (A→B) before reconnect.
        Board::GetInstance().RegenerateUuid();
        ESP_LOGI(TAG, "ApplyDeviceIdentity: new Client-Id=%s Device-Id=%s",
                 Board::GetInstance().GetUuid().c_str(), mac.c_str());

        if (has_ws) {
            ESP_LOGI(TAG, "ApplyDeviceIdentity: reopen via WebSocket (Device-Id header)");
        } else {
            PatchMqttIdentityToCurrentMac();
            ESP_LOGI(TAG, "ApplyDeviceIdentity: reopen via MQTT (patched client_id)");
        }

        InitializeProtocol(has_ws);

        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            display->SetChatMessage("system", "");
            display->ShowNotification("Đã đổi khóa", 2000);
        }

        // After stale CloseAudioChannel idle callback is drained, open like "Hi,Jason"
        // so the server greets and the mic is ready.
        Schedule([this]() {
            suppress_channel_closed_idle_ = false;
            std::string wake_word = audio_service_.GetLastWakeWord();
            if (wake_word.empty()) {
                wake_word = "Hi,Jason";
            }
            ESP_LOGI(TAG, "ApplyDeviceIdentity: auto greet via wake word '%s'", wake_word.c_str());
            WakeWordInvoke(wake_word);
        });
    });
}

#if CONFIG_SILENCE_PROMPT_ENABLE
void Application::ResetSilencePromptTracking() {
    const auto now = std::chrono::steady_clock::now();
    last_server_speech_activity_time_ = now;
    last_silence_prompt_send_time_ = {};
    last_silence_prompt_send_valid_ = false;
}

void Application::NotifyServerSpeechActivity() {
    last_server_speech_activity_time_ = std::chrono::steady_clock::now();
}

void Application::CheckSilencePrompt() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }
    if (!protocol_ || !protocol_->IsAudioChannelOpened()) {
        return;
    }
    if (audio_service_.IsLocalPlaybackActive() || audio_service_.IsCaptureSuspended()) {
        return;
    }
    if (silence_prompt_task_handle_ != nullptr) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto server_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_server_speech_activity_time_).count();
    const int timeout_ms = CONFIG_SILENCE_PROMPT_TIMEOUT_SEC * 1000;

    if (server_idle_ms < timeout_ms) {
        return;
    }

    if (last_silence_prompt_send_valid_) {
        const auto since_last_send_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_silence_prompt_send_time_).count();
        if (since_last_send_ms < timeout_ms) {
            return;
        }
    }

    SendSilencePromptAudio();
}

void Application::SendSilencePromptAudio() {
    if (silence_prompt_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Silence prompt send already in progress");
        return;
    }

    last_silence_prompt_send_time_ = std::chrono::steady_clock::now();
    last_silence_prompt_send_valid_ = true;
    ESP_LOGI(TAG, "Sending silence prompt uplink audio (no server STT/TTS for %d s)",
             CONFIG_SILENCE_PROMPT_TIMEOUT_SEC);

    audio_service_.EnableVoiceProcessing(false);
    audio_service_.ClearUplinkQueues();

    BaseType_t created = xTaskCreate([](void* arg) {
        auto* app = static_cast<Application*>(arg);
        vTaskDelay(pdMS_TO_TICKS(100));
        const auto [sent, total] = app->audio_service_.SendOggUplink(
            Lang::Sounds::OGG_SILENCE_PROMPT,
            [app](std::unique_ptr<AudioStreamPacket> packet) {
                return app->protocol_ && app->protocol_->SendAudio(std::move(packet));
            });
        app->Schedule([app, sent, total]() {
            app->silence_prompt_task_handle_ = nullptr;
            if (sent == 0 || sent < total) {
                ESP_LOGW(TAG, "Silence prompt incomplete (%u/%u), will retry after next idle",
                         (unsigned)sent, (unsigned)total);
                app->last_silence_prompt_send_valid_ = false;
            }
            if (app->GetDeviceState() == kDeviceStateListening &&
                !app->audio_service_.IsCaptureSuspended()) {
                app->audio_service_.EnableVoiceProcessing(true);
            }
        });
        vTaskDelete(nullptr);
    }, "silence_prompt", 4096, this, 2, &silence_prompt_task_handle_);
    if (created != pdPASS) {
        silence_prompt_task_handle_ = nullptr;
        last_silence_prompt_send_valid_ = false;
        ESP_LOGW(TAG, "Failed to start silence prompt send task");
        if (GetDeviceState() == kDeviceStateListening) {
            audio_service_.EnableVoiceProcessing(true);
        }
    }
}
#endif

