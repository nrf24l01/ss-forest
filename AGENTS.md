# Agent Architecture and Guidelines

This document outlines the architectural principles, structural requirements, and coding conventions for developing "Agents" within this ESP-IDF project. 

## What is an Agent?
In the context of this project, an **Agent** is an autonomous, encapsulated module responsible for a specific domain of the system (e.g., MQTT Agent, Sensor Agent, OTA Agent). Under the hood, an Agent is typically backed by a dedicated FreeRTOS task or operates as an event-driven state machine hooked into the `esp_event` loop.

Agents must be modular, thread-safe, and isolated. They should communicate with other parts of the system exclusively through thread-safe mechanisms like FreeRTOS Queues, Ringbuffers, or the ESP Event Loop.

---

## 1. Directory Structure

Agents should be implemented as standard ESP-IDF components. A typical agent component follows this structure:
```text
components/
└── agent_name/
    ├── CMakeLists.txt
    ├── include/
    │   └── agent_name.h        # Public API (Init, Start, Stop, structures)
    ├── src/
    │   └── agent_name.c        # Implementation (Task loop, private state)
    └── private_include/        # (Optional) Internal headers
```

---

## 2. Standard Agent Structure

To maintain consistency, all agents must encapsulate their state within a private context structure. Avoid global variables.

### The Context Struct
Define a context structure in your `.c` file (or a private header) to hold the agent's state, task handles, and synchronization primitives.
```c
// src/example_agent.c

typedef struct {
    TaskHandle_t        task_handle;
    QueueHandle_t       event_queue;
    SemaphoreHandle_t   lock;
    bool                is_running;
    
    // Agent-specific configuration and state
    uint32_t            poll_interval_ms;
    esp_mqtt_client_handle_t mqtt_client;
} example_agent_ctx_t;

// Singleton instance (if applicable) or dynamically allocated
static example_agent_ctx_t *s_ctx = NULL;
```

---

## 3. Agent Lifecycle

Every agent must expose a standard set of lifecycle functions in its public header (`.h` file). 

*   **`_init()`**: Allocates memory, creates queues, and initializes peripherals/mutexes. Does *not* start the FreeRTOS task.
*   **`_start()`**: Spawns the FreeRTOS task and begins background execution.
*   **`_stop()`**: Signals the task to shut down gracefully.
*   **`_deinit()`**: Frees memory and cleans up RTOS primitives after the task has stopped.

### API Example
```c
// include/example_agent.h

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t poll_interval_ms;
    int priority;
    int core_id;
} example_agent_config_t;

/**
 * @brief Initialize the example agent
 * @param config Pointer to configuration struct
 * @return ESP_OK on success, standard esp_err_t otherwise
 */
esp_err_t example_agent_init(const example_agent_config_t *config);

/**
 * @brief Start the agent's FreeRTOS task
 * @return ESP_OK on success
 */
esp_err_t example_agent_start(void);

/**
 * @brief Stop the agent's FreeRTOS task gracefully
 * @return ESP_OK on success
 */
esp_err_t example_agent_stop(void);

/**
 * @brief Free resources associated with the agent
 * @return ESP_OK on success
 */
esp_err_t example_agent_deinit(void);

#ifdef __cplusplus
}
#endif
```

---

## 4. Coding Style and Conventions

This project strictly adheres to the standard ESP-IDF programming guide, which is heavily based on the Linux Kernel coding style.

### Naming Conventions
*   **Functions & Variables:** Use `snake_case` (e.g., `process_sensor_data`).
*   **Macros & Enums:** Use `UPPER_SNAKE_CASE` (e.g., `AGENT_MAX_RETRIES`).
*   **Typedefs:** Suffix with `_t` (e.g., `sensor_reading_t`).
*   **Private Functions/Variables:** Prefix static/private elements with `s_` for static variables and `_` or `prv_` for static functions (e.g., `s_agent_state`, `prv_handle_event()`).

### Indentation and Braces
*   Use **4 spaces** for indentation. Do not use tabs.
*   Use K&R style brace placement. Opening braces go on the same line as the statement.
```c
// Correct
if (condition) {
    do_something();
} else {
    do_something_else();
}

// Incorrect
if(condition)
{
    do_something();
}
```

### Error Handling
*   Functions that can fail **must** return `esp_err_t`.
*   Use the `ESP_RETURN_ON_ERROR` and `ESP_GOTO_ON_ERROR` macros (from `esp_check.h`) to keep code clean and handle failures systematically.
```c
esp_err_t example_agent_init(const example_agent_config_t *config) {
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config cannot be null");
    
    s_ctx = calloc(1, sizeof(example_agent_ctx_t));
    ESP_RETURN_ON_FALSE(s_ctx != NULL, ESP_ERR_NO_MEM, TAG, "Failed to allocate context");

    s_ctx->event_queue = xQueueCreate(10, sizeof(agent_event_t));
    if (s_ctx->event_queue == NULL) {
        free(s_ctx);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
```

### Logging
*   Always use the `ESP_LOG*` macros (`ESP_LOGI`, `ESP_LOGE`, `ESP_LOGW`, `ESP_LOGD`). 
*   Never use `printf()` for agent logging.
*   Define a static `TAG` at the top of your `.c` file:
```c
static const char *TAG = "example_agent";

// Later in code...
ESP_LOGI(TAG, "Agent initialized successfully");
```

---

## 5. Concurrency and Thread Safety

*   **No Blocking in Callbacks:** If an agent hooks into an interrupt (ISR) or a system event loop callback, **never** perform blocking operations (like `vTaskDelay` or taking a blocking mutex). Instead, push an event to the agent's FreeRTOS queue and process it in the agent's main task loop.
*   **Data Protection:** If multiple tasks must read/write to the agent's state, use a FreeRTOS `SemaphoreHandle_t` (Mutex) to protect the critical sections. Keep the locked section as short as possible.
```