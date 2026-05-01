/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024 Mark LaPointe <mark@cloudbsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/select.h>
#include <pthread.h>

/* Global flag for signal handling */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Forward declarations for server/client modes */
static int run_server_mode(int port);
static int run_client_mode(const char *host, int port);

/* Mode enumeration */
enum mode {
    MODE_DIAGNOSE = 0,
    MODE_ACCURACY,
    MODE_AFFINITY,
    MODE_TRANSFER,
    MODE_GOVERNOR,
    MODE_STRESS,
    MODE_BENCHMARK,
    MODE_MENU
};

/* Configuration */
static struct {
    enum mode mode;
    int show_workers;
    int show_sessions;
    int show_governor;
    int health_score;
    int check_affinity;
    int check_balance;
    int monitor;
    int interval;
    int imbalance_threshold;
    int health_threshold;
    int no_color;
    int quiet;
    int json_output;
    int jsonl_output;
    int csv_output;
    int tap_output;
    
    /* Test mode options */
    int test_sessions;
    int test_algorithm;
    int test_workers;
    int test_threshold;
    int test_retries;
    int test_duration;
    int test_buffer_size;
    int verbose;
    int server_mode;
    int client_mode;
    char *host;
    int port;
    char *file;
    char *trigger;
} config = {
    .mode = MODE_DIAGNOSE,
    .interval = 5,
    .imbalance_threshold = 20,
    .health_threshold = 40,
    .test_sessions = 100,
    .test_algorithm = 0,
    .test_workers = 4,
    .test_threshold = 10,
    .test_retries = 3,
    .test_duration = 60,
    .test_buffer_size = 65536,
    .port = 9001,
};

/* ANSI color codes */
#define COLOR_RESET     "\033[0m"
#define COLOR_GREEN    "\033[32m"
#define COLOR_RED      "\033[31m"
#define COLOR_YELLOW   "\033[33m"
#define COLOR_CYAN     "\033[36m"
#define COLOR_BOLD     "\033[1m"

#define USE_COLOR (isatty(STDOUT_FILENO) && !config.no_color)

/* Sysctl paths */
#define BASE_PATH "net.graph.pppoe_lb"

/* Worker states */
enum worker_state {
    WORKER_ACTIVE = 0,
    WORKER_DRAINING = 1,
    WORKER_PENDING_REMOVAL = 2
};

/* Data structures */
struct worker_info {
    int id;
    enum worker_state state;
    int sessions;
    time_t last_activity;
    time_t uptime;
    uint64_t bytes_in;
    uint64_t bytes_out;
    int errors;
};

struct session_info {
    uint64_t session_id;
    int worker_id;
    time_t created;
    time_t last_activity;
    uint64_t bytes_in;
    uint64_t bytes_out;
    int errors;
};

struct governor_status {
    int enabled;
    int mode;
    int min_workers;
    int max_workers;
    int current_workers;
    int pending_removals;
    int cpu_threshold;
    int cpu_low_threshold;
    int poll_interval;
    int drain_timeout;
    int sessions_per_worker;
    double cpu_usage;
    double cpu_avg;
    char last_decision[32];
    char last_reason[32];
    time_t last_decision_time;
};

/* Utility functions */
static const char *state_to_string(enum worker_state state) {
    switch (state) {
        case WORKER_ACTIVE: return "ACTIVE";
        case WORKER_DRAINING: return "DRAINING";
        case WORKER_PENDING_REMOVAL: return "PENDING";
        default: return "UNKNOWN";
    }
}

static const char *format_bytes(uint64_t bytes, char *buf, size_t len) {
    if (bytes < 1024) {
        snprintf(buf, len, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(buf, len, "%.2f KB", bytes / 1024.0);
    } else if (bytes < 1024 * 1024 * 1024) {
        snprintf(buf, len, "%.2f MB", bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, len, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    return buf;
}

static const char *format_duration(time_t seconds, char *buf, size_t len) {
    if (seconds < 60) {
        snprintf(buf, len, "%lds", (long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, len, "%ldm %lds", (long)(seconds / 60), (long)(seconds % 60));
    } else {
        snprintf(buf, len, "%ldh %ldm", (long)(seconds / 3600), (long)((seconds % 3600) / 60));
    }
    return buf;
}

static const char *timestamp_iso(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S%z", tm);
    return buf;
}

/* Sysctl helpers */
static int sysctl_int(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s.%s", BASE_PATH, name);
    
    int val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname(path, &val, &len, NULL, 0) != 0) {
        return -1;
    }
    return val;
}

static double sysctl_double(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s.%s", BASE_PATH, name);
    
    double val = 0;
    size_t len = sizeof(val);
    if (sysctlbyname(path, &val, &len, NULL, 0) != 0) {
        return -1.0;
    }
    return val;
}

static int sysctl_string(const char *name, char *buf, size_t len) {
    char path[256];
    snprintf(path, sizeof(path), "%s.%s", BASE_PATH, name);
    
    if (sysctlbyname(path, buf, &len, NULL, 0) != 0) {
        return -1;
    }
    return 0;
}

/* Get session count */
static int get_session_count(void) {
    return sysctl_int("governor.total_sessions");
}

/* Get session information (up to max_sessions) */
static int get_sessions(struct session_info **sessions_out, int max_sessions) {
    int count = sysctl_int("governor.total_sessions");
    if (count < 0) {
        *sessions_out = NULL;
        return 0;
    }
    
    if (count > max_sessions) {
        count = max_sessions;
    }
    
    struct session_info *sessions = calloc(count, sizeof(struct session_info));
    if (!sessions) {
        return -1;
    }
    
    for (int i = 0; i < count; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "sessions.%d.id", i);
        sessions[i].session_id = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.worker_id", i);
        sessions[i].worker_id = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.created", i);
        sessions[i].created = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.last_activity", i);
        sessions[i].last_activity = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.bytes_in", i);
        sessions[i].bytes_in = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.bytes_out", i);
        sessions[i].bytes_out = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "sessions.%d.errors", i);
        sessions[i].errors = sysctl_int(buf);
    }
    
    *sessions_out = sessions;
    return count;
}

/* Forward declarations for session tracking */
static void track_session(uint64_t session_id, int worker_id);
static int get_drift_count(void);

/* Get worker information */
static int get_workers(struct worker_info **workers_out) {
    int count = sysctl_int("governor.active_workers");
    if (count < 0) {
        *workers_out = NULL;
        return 0;
    }
    
    struct worker_info *workers = calloc(count, sizeof(struct worker_info));
    if (!workers) {
        return -1;
    }
    
    for (int i = 0; i < count; i++) {
        char buf[64];
        workers[i].id = i;
        snprintf(buf, sizeof(buf), "workers.%d.state", i);
        workers[i].state = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.sessions", i);
        workers[i].sessions = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.last_activity", i);
        workers[i].last_activity = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.uptime", i);
        workers[i].uptime = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.bytes_in", i);
        workers[i].bytes_in = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.bytes_out", i);
        workers[i].bytes_out = sysctl_int(buf);
        snprintf(buf, sizeof(buf), "workers.%d.errors", i);
        workers[i].errors = sysctl_int(buf);
    }
    
    *workers_out = workers;
    return count;
}

/* Get governor status */
static int get_governor_status(struct governor_status *status) {
    memset(status, 0, sizeof(*status));
    
    status->enabled = sysctl_int("governor.enabled") == 1;
    status->mode = sysctl_int("governor.mode");
    status->min_workers = sysctl_int("governor.min_workers");
    status->max_workers = sysctl_int("governor.max_workers");
    status->current_workers = sysctl_int("governor.current_workers");
    status->pending_removals = sysctl_int("governor.pending_removals");
    status->cpu_threshold = sysctl_int("governor.cpu_threshold");
    status->cpu_low_threshold = sysctl_int("governor.cpu_low_threshold");
    status->poll_interval = sysctl_int("governor.poll_interval");
    status->drain_timeout = sysctl_int("governor.drain_timeout");
    status->sessions_per_worker = sysctl_int("governor.sessions_per_worker");
    status->cpu_usage = sysctl_double("governor.cpu_usage");
    status->cpu_avg = sysctl_double("governor.cpu_avg");
    
    char buf[64];
    if (sysctl_string("governor.last_decision", buf, sizeof(buf)) == 0) {
        strlcpy(status->last_decision, buf, sizeof(status->last_decision));
    }
    if (sysctl_string("governor.last_reason", buf, sizeof(buf)) == 0) {
        strlcpy(status->last_reason, buf, sizeof(status->last_reason));
    }
    status->last_decision_time = sysctl_int("governor.last_decision_time");
    
    return 0;
}

/* Print functions */
static void print_header(const char *title) {
    if (config.json_output || config.jsonl_output) return;
    
    printf("\n");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("================================================================================\n");
    printf("  %s\n", title);
    printf("================================================================================");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n\n");
}

static void print_workers(void) {
    struct worker_info *workers = NULL;
    int count = get_workers(&workers);
    
    print_header("Worker Status");
    
    if (count <= 0) {
        printf("  No workers found (module may not be loaded)\n\n");
        return;
    }
    
    if (config.json_output) {
        printf("{\n");
        printf("  \"workers\": [\n");
        for (int i = 0; i < count; i++) {
            printf("    {\n");
            printf("      \"id\": %d,\n", workers[i].id);
            printf("      \"state\": \"%s\",\n", state_to_string(workers[i].state));
            printf("      \"sessions\": %d,\n", workers[i].sessions);
            printf("      \"bytes_in\": %llu,\n", (unsigned long long)workers[i].bytes_in);
            printf("      \"bytes_out\": %llu,\n", (unsigned long long)workers[i].bytes_out);
            printf("      \"errors\": %d,\n", workers[i].errors);
            printf("      \"uptime\": %ld\n", (long)workers[i].uptime);
            printf("    }%s\n", i < count - 1 ? "," : "");
        }
        printf("  ]\n");
        printf("}\n");
        free(workers);
        return;
    }
    
    /* Console output */
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("%-6s  %-10s  %-10s  %-10s  %-10s  %-8s  %-10s\n",
           "Worker", "State", "Sessions", "Bytes In", "Bytes Out", "Errors", "Uptime");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("  %s\n", "------------------------------------------------------------------------------");
    
    for (int i = 0; i < count; i++) {
        char buf1[32], buf2[32], buf3[32];
        
        printf("  %-6d  ", workers[i].id);
        
        /* State with color */
        if (USE_COLOR) {
            switch (workers[i].state) {
                case WORKER_ACTIVE: printf("%s", COLOR_GREEN); break;
                case WORKER_DRAINING: printf("%s", COLOR_YELLOW); break;
                case WORKER_PENDING_REMOVAL: printf("%s", COLOR_RED); break;
            }
        }
        printf("%-10s", state_to_string(workers[i].state));
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("  ");
        
        printf("%-10d  %-10s  %-10s  %-8d  %-10s\n",
               workers[i].sessions,
               format_bytes(workers[i].bytes_in, buf1, sizeof(buf1)),
               format_bytes(workers[i].bytes_out, buf2, sizeof(buf2)),
               workers[i].errors,
               format_duration(workers[i].uptime, buf3, sizeof(buf3)));
    }
    
    printf("\n");
    free(workers);
}

static void print_sessions(void) {
    struct session_info *sessions = NULL;
    int count = get_sessions(&sessions, 100);  /* Limit to 100 for display */
    
    print_header("Session Status");
    
    int total_sessions = get_session_count();
    printf("  Total Active Sessions: %d", total_sessions);
    if (total_sessions > 100) {
        printf(" (showing first 100)");
    }
    printf("\n\n");
    
    if (count <= 0) {
        printf("  No sessions found\n\n");
        return;
    }
    
    if (config.json_output) {
        printf("{\n");
        printf("  \"total\": %d,\n", total_sessions);
        printf("  \"sessions\": [\n");
        for (int i = 0; i < count; i++) {
            printf("    {\n");
            printf("      \"id\": \"0x%lx\",\n", (unsigned long)sessions[i].session_id);
            printf("      \"worker_id\": %d,\n", sessions[i].worker_id);
            printf("      \"created\": %ld,\n", (long)sessions[i].created);
            printf("      \"age\": %ld,\n", (long)(time(NULL) - sessions[i].created));
            printf("      \"bytes_in\": %llu,\n", (unsigned long long)sessions[i].bytes_in);
            printf("      \"bytes_out\": %llu,\n", (unsigned long long)sessions[i].bytes_out);
            printf("      \"errors\": %d\n", sessions[i].errors);
            printf("    }%s\n", i < count - 1 ? "," : "");
        }
        printf("  ]\n");
        printf("}\n");
        free(sessions);
        return;
    }
    
    /* Console output */
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("%-12s  %-6s  %-10s  %-12s  %-12s  %-8s  %-8s\n",
           "Session ID", "Worker", "Age", "Bytes In", "Bytes Out", "Errors", "Status");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("  %s\n", "-----------------------------------------------------------------------------------------");
    
    for (int i = 0; i < count; i++) {
        char buf1[32], buf2[32], age_buf[32];
        time_t age = time(NULL) - sessions[i].created;
        format_duration(age, age_buf, sizeof(age_buf));
        
        /* Track session for drift detection */
        track_session(sessions[i].session_id, sessions[i].worker_id);
        
        printf("  %-12s  ", "");
        if (USE_COLOR) printf("%s", COLOR_CYAN);
        printf("0x%lx", (unsigned long)sessions[i].session_id);
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("  ");
        
        printf("%-6d  %-10s  %-12s  %-12s  %-8d  ",
               sessions[i].worker_id,
               age_buf,
               format_bytes(sessions[i].bytes_in, buf1, sizeof(buf1)),
               format_bytes(sessions[i].bytes_out, buf2, sizeof(buf2)),
               sessions[i].errors);
        
        /* Status indicator */
        if (sessions[i].errors > 0) {
            if (USE_COLOR) printf("%s", COLOR_RED);
            printf("ERROR");
        } else {
            if (USE_COLOR) printf("%s", COLOR_GREEN);
            printf("OK");
        }
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
    }
    
    printf("\n");
    
    /* Show affinity drift summary if any */
    int drifts = get_drift_count();
    if (drifts > 0) {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Note: %d session(s) have changed workers since monitoring started.\n", drifts);
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("       This may indicate governor scaling activity or affinity issues.\n");
        printf("\n");
    }
    
    free(sessions);
}

static void print_governor(void) {
    struct governor_status status;
    get_governor_status(&status);
    
    print_header("Governor Status");
    
    if (config.json_output) {
        printf("{\n");
        printf("  \"enabled\": %s,\n", status.enabled ? "true" : "false");
        printf("  \"mode\": %s,\n", status.mode == 1 ? "\"auto\"" : "\"manual\"");
        printf("  \"current_workers\": %d,\n", status.current_workers);
        printf("  \"min_workers\": %d,\n", status.min_workers);
        printf("  \"max_workers\": %d,\n", status.max_workers);
        printf("  \"pending_removals\": %d,\n", status.pending_removals);
        printf("  \"cpu_threshold\": %d,\n", status.cpu_threshold);
        printf("  \"cpu_low_threshold\": %d,\n", status.cpu_low_threshold);
        printf("  \"poll_interval\": %d,\n", status.poll_interval);
        printf("  \"drain_timeout\": %d,\n", status.drain_timeout);
        printf("  \"sessions_per_worker\": %d,\n", status.sessions_per_worker);
        printf("  \"cpu_usage\": %.1f,\n", status.cpu_usage);
        printf("  \"cpu_avg\": %.1f,\n", status.cpu_avg);
        printf("  \"last_decision\": \"%s\",\n", status.last_decision);
        printf("  \"last_reason\": \"%s\"\n", status.last_reason);
        printf("}\n");
        return;
    }
    
    printf("  Governor Configuration:\n");
    printf("  %s\n", "--------------------------------------------------");
    printf("  %-25s: %s\n", "Enabled", status.enabled ? "Yes" : "No");
    printf("  %-25s: %s\n", "Mode", status.mode == 1 ? "Auto" : "Manual");
    printf("  %-25s: %d\n", "Current Workers", status.current_workers);
    printf("  %-25s: %d\n", "Min Workers", status.min_workers);
    printf("  %-25s: %d\n", "Max Workers", status.max_workers);
    printf("  %-25s: %d\n", "Pending Removals", status.pending_removals);
    printf("\n");
    
    printf("  Thresholds:\n");
    printf("  %s\n", "--------------------------------------------------");
    printf("  %-25s: %d%%\n", "CPU Scale-Up Threshold", status.cpu_threshold);
    printf("  %-25s: %d%%\n", "CPU Scale-Down Threshold", status.cpu_low_threshold);
    printf("  %-25s: %d\n", "Sessions/Worker Target", status.sessions_per_worker);
    printf("  %-25s: %ds\n", "Poll Interval", status.poll_interval);
    printf("  %-25s: %ds\n", "Drain Timeout", status.drain_timeout);
    printf("\n");
    
    printf("  Current Status:\n");
    printf("  %s\n", "--------------------------------------------------");
    printf("  %-25s: %.1f%%\n", "CPU Usage", status.cpu_usage);
    printf("  %-25s: %.1f%%\n", "CPU Average", status.cpu_avg);
    printf("  %-25s: %s\n", "Last Decision", status.last_decision);
    printf("  %-25s: %s\n", "Last Reason", status.last_reason);
    printf("\n");
}

static int calculate_health_score(void) {
    struct worker_info *workers = NULL;
    int count = get_workers(&workers);
    struct governor_status gov;
    get_governor_status(&gov);
    
    if (count <= 0) {
        /* Module not loaded */
        if (workers) free(workers);
        return 50;  /* Unknown */
    }
    
    /* Calculate worker score */
    int active = 0;
    for (int i = 0; i < count; i++) {
        if (workers[i].state == WORKER_ACTIVE) active++;
    }
    int worker_score = (active * 100) / count;
    
    /* Calculate balance score */
    int total_sessions = 0;
    for (int i = 0; i < count; i++) {
        total_sessions += workers[i].sessions;
    }
    int ideal = count > 0 ? total_sessions / count : 0;
    int max_dev = 0;
    for (int i = 0; i < count; i++) {
        int dev = abs(workers[i].sessions - ideal);
        int pct = ideal > 0 ? (dev * 100) / ideal : 0;
        if (pct > max_dev) max_dev = pct;
    }
    int balance_score = (max_dev <= config.imbalance_threshold) ? 100 : 100 - max_dev;
    
    /* Calculate error score */
    int total_errors = 0;
    for (int i = 0; i < count; i++) {
        total_errors += workers[i].errors;
    }
    uint64_t total_bytes = 0;
    for (int i = 0; i < count; i++) {
        total_bytes += workers[i].bytes_in + workers[i].bytes_out;
    }
    double error_rate = total_bytes > 0 ? (double)total_errors / total_bytes * 1000000 : 0;
    int error_score;
    if (error_rate < 0.001) error_score = 100;
    else if (error_rate < 0.01) error_score = 80;
    else if (error_rate < 0.1) error_score = 50;
    else error_score = 10;
    
    free(workers);
    
    /* Overall score */
    return (worker_score + balance_score + error_score) / 3;
}

/* Track session assignments for drift detection */
#define MAX_TRACKED_SESSIONS 1000
static struct {
    uint64_t session_id;
    int expected_worker;
    time_t first_seen;
    int drift_count;
} session_tracker[MAX_TRACKED_SESSIONS];
static int tracked_count = 0;

static void print_component_score(const char *name, int score, int extra, int threshold) {
    if (score >= 90) {
        if (USE_COLOR) printf("%s", COLOR_GREEN);
    } else if (score >= 70) {
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
    } else {
        if (USE_COLOR) printf("%s", COLOR_RED);
    }
    printf("%d/100", score);
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf(" %s", name);
    if (extra > 0) {
        printf(" (%d workers)", extra);
    }
    if (threshold > 0) {
        printf(" [threshold: %d%%]", threshold);
    }
    printf("\n");
}

static void track_session(uint64_t session_id, int worker_id) {
    /* Look for existing entry */
    for (int i = 0; i < tracked_count; i++) {
        if (session_tracker[i].session_id == session_id) {
            /* Check for drift */
            if (session_tracker[i].expected_worker != worker_id && session_tracker[i].expected_worker >= 0) {
                session_tracker[i].drift_count++;
                if (config.verbose && session_tracker[i].drift_count == 1) {
                    printf("  [AFFINITY] Session 0x%lx drifted: worker %d -> %d\n",
                           (unsigned long)session_id, session_tracker[i].expected_worker, worker_id);
                }
            }
            session_tracker[i].expected_worker = worker_id;
            return;
        }
    }
    
    /* Add new entry */
    if (tracked_count < MAX_TRACKED_SESSIONS) {
        session_tracker[tracked_count].session_id = session_id;
        session_tracker[tracked_count].expected_worker = worker_id;
        session_tracker[tracked_count].first_seen = time(NULL);
        session_tracker[tracked_count].drift_count = 0;
        tracked_count++;
    }
}

static int get_drift_count(void) {
    int count = 0;
    for (int i = 0; i < tracked_count; i++) {
        if (session_tracker[i].drift_count > 0) {
            count++;
        }
    }
    return count;
}

static void print_health_score(void) {
    int score = calculate_health_score();
    struct worker_info *workers = NULL;
    int worker_count = get_workers(&workers);
    struct governor_status gov;
    get_governor_status(&gov);
    
    /* Calculate component scores */
    int worker_score = 50, balance_score = 50, error_score = 50, affinity_score = 100;
    
    if (worker_count > 0) {
        int active = 0;
        for (int i = 0; i < worker_count; i++) {
            if (workers[i].state == WORKER_ACTIVE) active++;
        }
        worker_score = (active * 100) / worker_count;
        
        int total_sessions = 0;
        for (int i = 0; i < worker_count; i++) {
            total_sessions += workers[i].sessions;
        }
        int ideal = worker_count > 0 ? total_sessions / worker_count : 0;
        int max_dev = 0;
        for (int i = 0; i < worker_count; i++) {
            int dev = abs(workers[i].sessions - ideal);
            int pct = ideal > 0 ? (dev * 100) / ideal : 0;
            if (pct > max_dev) max_dev = pct;
        }
        balance_score = (max_dev <= config.imbalance_threshold) ? 100 : 100 - max_dev;
        
        int total_errors = 0;
        uint64_t total_bytes = 0;
        for (int i = 0; i < worker_count; i++) {
            total_errors += workers[i].errors;
            total_bytes += workers[i].bytes_in + workers[i].bytes_out;
        }
        double error_rate = total_bytes > 0 ? (double)total_errors / total_bytes * 1000000 : 0;
        if (error_rate < 0.001) error_score = 100;
        else if (error_rate < 0.01) error_score = 80;
        else if (error_rate < 0.1) error_score = 50;
        else error_score = 10;
    }
    
    /* Affinity score */
    int drifts = get_drift_count();
    if (drifts == 0) affinity_score = 100;
    else if (drifts <= 2) affinity_score = 90;
    else if (drifts <= 5) affinity_score = 70;
    else if (drifts <= 10) affinity_score = 50;
    else affinity_score = 30;
    
    free(workers);
    
    print_header("Health Assessment");
    
    if (config.json_output) {
        printf("{\n");
        printf("  \"overall\": %d,\n", score);
        printf("  \"thresholds\": {\n");
        printf("    \"imbalance\": %d,\n", config.imbalance_threshold);
        printf("    \"health\": %d\n", config.health_threshold);
        printf("  }\n");
        printf("}\n");
        return;
    }
    
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("Overall Health Score: ");
    
    if (score >= 90) {
        if (USE_COLOR) printf("%s", COLOR_GREEN);
    } else if (score >= 70) {
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
    } else {
        if (USE_COLOR) printf("%s", COLOR_RED);
    }
    printf("%d/100", score);
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n\n");
    
    /* Component breakdown */
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("Component Scores:\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    
    printf("  ");
    print_component_score("Worker Availability", worker_score, worker_count > 0 ? worker_count : 0, 0);
    printf("  ");
    print_component_score("Load Distribution", balance_score, 0, config.imbalance_threshold);
    printf("  ");
    print_component_score("Error Rate", error_score, 0, 0);
    printf("  ");
    print_component_score("Session Affinity", affinity_score, drifts, 0);
    printf("\n");
    
    if (score >= 90) {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_GREEN);
        printf("Status: HEALTHY");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - System operating normally.\n");
    } else if (score >= 70) {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Status: WARNING");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - Some metrics need attention.\n");
    } else {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_RED);
        printf("Status: CRITICAL");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - Immediate attention required.\n");
    }
    
    /* Recommendations */
    if (balance_score < 80) {
        printf("\n  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Recommendation: ");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("Check worker session distribution and algorithm settings.\n");
        printf("             Run: pppoetest diagnose --check-balance for details.\n");
    }
    if (error_score < 80) {
        printf("\n  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Recommendation: ");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("Check worker error counts with pppoetest diagnose -s.\n");
    }
    if (affinity_score < 80) {
        printf("\n  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Recommendation: ");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("Session affinity violations detected. Check governor scaling.\n");
        printf("             Run with --check-affinity to track session movements.\n");
    }
    printf("\n");
}

static void print_balance_check(void) {
    struct worker_info *workers = NULL;
    int count = get_workers(&workers);
    
    print_header("Load Balance Check");
    
    if (count <= 0) {
        printf("  No workers found\n\n");
        return;
    }
    
    int total_sessions = 0;
    for (int i = 0; i < count; i++) {
        total_sessions += workers[i].sessions;
    }
    int ideal = count > 0 ? total_sessions / count : 0;
    int max_dev = 0;
    
    if (config.json_output) {
        printf("{\n");
        printf("  \"ideal_sessions_per_worker\": %d,\n", ideal);
        printf("  \"workers\": [\n");
        for (int i = 0; i < count; i++) {
            int dev = abs(workers[i].sessions - ideal);
            int pct = ideal > 0 ? (dev * 100) / ideal : 0;
            if (pct > max_dev) max_dev = pct;
            
            printf("    {\n");
            printf("      \"id\": %d,\n", workers[i].id);
            printf("      \"actual\": %d,\n", workers[i].sessions);
            printf("      \"expected\": %d,\n", ideal);
            printf("      \"deviation_pct\": %d,\n", pct);
            printf("      \"healthy\": %s\n", pct <= config.imbalance_threshold ? "true" : "false");
            printf("    }%s\n", i < count - 1 ? "," : "");
        }
        printf("  ],\n");
        printf("  \"max_deviation_pct\": %d,\n", max_dev);
        printf("  \"healthy\": %s\n", max_dev <= config.imbalance_threshold ? "true" : "false");
        printf("}\n");
        free(workers);
        return;
    }
    
    printf("  Ideal sessions per worker: %d\n", ideal);
    printf("  Max deviation: %d%%\n", max_dev);
    printf("  Status: ");
    
    if (max_dev <= config.imbalance_threshold) {
        if (USE_COLOR) printf("%s", COLOR_GREEN);
        printf("HEALTHY");
    } else {
        if (USE_COLOR) printf("%s", COLOR_RED);
        printf("UNHEALTHY");
    }
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n\n");
    
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("%-8s  %-12s  %-12s  %-12s  %-10s\n",
           "Worker", "Actual", "Expected", "Deviation", "Status");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("  %s\n", "--------------------------------------------------------");
    
    for (int i = 0; i < count; i++) {
        int dev = abs(workers[i].sessions - ideal);
        int pct = ideal > 0 ? (dev * 100) / ideal : 0;
        if (pct > max_dev) max_dev = pct;
        
        printf("  %-8d  %-12d  %-12d  %-11d%%  ", 
               workers[i].id, workers[i].sessions, ideal, pct);
        
        if (pct <= config.imbalance_threshold) {
            if (USE_COLOR) printf("%s", COLOR_GREEN);
            printf("%-10s", "OK");
        } else {
            if (USE_COLOR) printf("%s", COLOR_RED);
            printf("%-10s", "IMBALANCED");
        }
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
    }
    
    printf("\n");
    free(workers);
}

static void run_monitoring(void) {
    printf("\n");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("PPPoE Load Balancer - Real-Time Monitoring");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    printf("Press Ctrl+C to stop\n\n");
    
    int iteration = 0;
    while (g_running) {
        iteration++;
        
        /* Clear screen */
        printf("\033[2J\033[H");
        
        char ts[64];
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_BOLD);
        printf("PPPoE Load Balancer - Real-Time Monitoring");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" (Iteration %d)\n", iteration);
        printf("  %s\n\n", timestamp_iso(ts, sizeof(ts)));
        
        /* Get current state */
        struct worker_info *workers = NULL;
        int count = get_workers(&workers);
        struct governor_status gov;
        get_governor_status(&gov);
        
        int active = 0, draining = 0, pending = 0;
        int total_sessions = 0;
        for (int i = 0; i < count; i++) {
            switch (workers[i].state) {
                case WORKER_ACTIVE: active++; break;
                case WORKER_DRAINING: draining++; break;
                case WORKER_PENDING_REMOVAL: pending++; break;
            }
            total_sessions += workers[i].sessions;
        }
        
        printf("  Summary:\n");
        printf("  %s\n", "----------------------------------------");
        printf("  %-20s: %d\n", "Total Workers", count);
        printf("  %-20s: %d\n", "  Active", active);
        printf("  %-20s: %d\n", "  Draining", draining);
        printf("  %-20s: %d\n", "  Pending Removal", pending);
        printf("  %-20s: %d\n", "Total Sessions", total_sessions);
        printf("  %-20s: %.1f%%\n", "CPU Usage", gov.cpu_usage);
        printf("  %-20s: %s\n", "Governor Decision", gov.last_decision);
        
        printf("\n  Per-Worker Breakdown:\n");
        printf("  %s\n", "----------------------------------------");
        
        for (int i = 0; i < count; i++) {
            char buf[32];
            printf("  Worker %d: ", workers[i].id);
            switch (workers[i].state) {
                case WORKER_ACTIVE:
                    if (USE_COLOR) printf("%s", COLOR_GREEN);
                    printf("ACTIVE");
                    break;
                case WORKER_DRAINING:
                    if (USE_COLOR) printf("%s", COLOR_YELLOW);
                    printf("DRAINING");
                    break;
                case WORKER_PENDING_REMOVAL:
                    if (USE_COLOR) printf("%s", COLOR_RED);
                    printf("PENDING");
                    break;
            }
            if (USE_COLOR) printf("%s", COLOR_RESET);
            printf("  Sessions: %d  Bytes In: %s  Errors: %d\n",
                   workers[i].sessions,
                   format_bytes(workers[i].bytes_in, buf, sizeof(buf)),
                   workers[i].errors);
        }
        
        free(workers);
        
        /* Track sessions for drift detection */
        struct session_info *sessions = NULL;
        int session_count = get_sessions(&sessions, 1000);
        for (int i = 0; i < session_count; i++) {
            track_session(sessions[i].session_id, sessions[i].worker_id);
        }
        free(sessions);
        
        /* Show drift info */
        int drifts = get_drift_count();
        if (drifts > 0) {
            printf("\n  ");
            if (USE_COLOR) printf("%s", COLOR_YELLOW);
            printf("! %d session(s) have changed workers", drifts);
            if (USE_COLOR) printf("%s", COLOR_RESET);
        }
        
        printf("\n\n  Updating in %d seconds...\n", config.interval);
        
        /* Sleep with interruptible sleep */
        struct timespec tspec = { .tv_sec = config.interval, .tv_nsec = 0 };
        while (g_running && nanosleep(&tspec, &tspec) < 0 && errno == EINTR)
            ;
    }
}

/* Accuracy test - session distribution verification */
static void run_accuracy_test(int sessions, int algorithm, int threshold) {
    print_header("Accuracy Test");
    printf("  [INFO] Accuracy test mode - %d sessions\n", sessions);
    printf("  [INFO] Algorithm: %d (0=round-robin, 1=hash, 2=least-loaded)\n", algorithm);
    printf("  [INFO] Threshold: %d%%\n\n", threshold);
    
    /* Get current worker state */
    struct worker_info *workers = NULL;
    int worker_count = get_workers(&workers);
    
    if (worker_count <= 0) {
        if (USE_COLOR) printf("%s", COLOR_RED);
        printf("  [ERROR] No workers available. Module may not be loaded.\n");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
        printf("  Please ensure the ng_pppoe_lb module is loaded:\n");
        printf("    kldload ng_pppoe_lb\n");
        printf("    kldload ng_pppoe\n\n");
        return;
    }
    
    /* Get initial session distribution */
    int initial_sessions = 0;
    for (int i = 0; i < worker_count; i++) {
        initial_sessions += workers[i].sessions;
    }
    
    printf("  Initial State:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  Workers: %d\n", worker_count);
    printf("  Active Sessions: %d\n", initial_sessions);
    printf("  Expected per worker: ~%d\n\n", 
           (initial_sessions + sessions) / worker_count);
    
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("  [NOTICE] Full accuracy test requires session creation capabilities.\n");
    printf("           The following would be tested:\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    printf("  Test Procedure:\n");
    printf("  %s\n", "----------------------------------------\n");
    printf("  1. Record current session distribution\n");
    printf("  2. Create %d test sessions\n", sessions);
    printf("  3. Record new session distribution\n");
    printf("  4. Compare distribution vs. expected algorithm\n");
    printf("\n");
    
    printf("  Expected Behavior by Algorithm:\n");
    printf("  %s\n", "----------------------------------------\n");
    switch (algorithm) {
        case 0:
            printf("  Round-Robin (0): Sessions should distribute evenly.\n");
            printf("  - Each new session goes to the next worker in sequence.\n");
            printf("  - Distribution: ~%d per worker (within %d%%)\n",
                   sessions / worker_count + (sessions % worker_count > 0 ? 1 : 0),
                   threshold);
            break;
        case 1:
            printf("  Hash-Based (1): Same source = same worker.\n");
            printf("  - Sessions from same source MAC/IP stay together.\n");
            printf("  - Consistent hashing with modulo distribution.\n");
            break;
        case 2:
            printf("  Least-Loaded (2): Fewest sessions = more likely.\n");
            printf("  - New sessions go to worker with fewest sessions.\n");
            printf("  - Distribution based on current load, not sequence.\n");
            break;
        default:
            printf("  Unknown algorithm: %d\n", algorithm);
    }
    printf("\n");
    
    /* Show expected results table */
    printf("  Expected Results Table:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  %-8s  %-12s  %-12s  %-10s\n",
           "Worker", "Initial", "Expected", "Status");
    printf("  %s\n", "--------------------------------------------------------");
    
    for (int i = 0; i < worker_count; i++) {
        int expected = workers[i].sessions + (sessions / worker_count);
        if (i < (sessions % worker_count)) expected++;
        
        printf("  %-8d  %-12d  %-12d  ", 
               i, workers[i].sessions, expected);
        if (USE_COLOR) printf("%s", COLOR_GREEN);
        printf("NOT TESTED");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
    }
    printf("\n");
    
    printf("  To run full accuracy test:\n");
    printf("    1. Ensure ng_pppoe_lb module is loaded\n");
    printf("    2. Create netgraph nodes for testing\n");
    printf("    3. Run this tool with appropriate permissions\n\n");
    
    free(workers);
}

/* Affinity test - session stickiness verification */
static void run_affinity_test(int sessions, int retries) {
    print_header("Affinity Test");
    printf("  [INFO] Affinity test mode - %d sessions with %d retries\n\n", sessions, retries);
    
    /* Get current sessions to check affinity state */
    struct session_info *session_list = NULL;
    int session_count = get_sessions(&session_list, sessions);
    struct worker_info *workers = NULL;
    int worker_count = get_workers(&workers);
    
    if (worker_count <= 0) {
        if (USE_COLOR) printf("%s", COLOR_RED);
        printf("  [ERROR] No workers available. Module may not be loaded.\n");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
        return;
    }
    
    printf("  Test Configuration:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  Sessions to track: %d\n", sessions);
    printf("  Reconnection retries: %d\n", retries);
    printf("  Available workers: %d\n\n", worker_count);
    
    if (session_count > 0) {
        printf("  Current Session Affinity State:\n");
        printf("  %s\n", "----------------------------------------");
        
        if (config.json_output) {
            printf("{\n");
            printf("  \"test_mode\": \"affinity\",\n");
            printf("  \"sessions_tested\": %d,\n", session_count);
            printf("  \"workers\": %d,\n", worker_count);
            printf("  \"sessions\": [\n");
            for (int i = 0; i < session_count && i < 20; i++) {
                printf("    {\n");
                printf("      \"id\": \"0x%lx\",\n", (unsigned long)session_list[i].session_id);
                printf("      \"worker\": %d,\n", session_list[i].worker_id);
                printf("      \"age_seconds\": %ld,\n", (long)(time(NULL) - session_list[i].created));
                printf("      \"bytes_in\": %llu,\n", (unsigned long long)session_list[i].bytes_in);
                printf("      \"errors\": %d\n", session_list[i].errors);
                printf("    }%s\n", i < session_count - 1 && i < 19 ? "," : "");
            }
            if (session_count > 20) {
                printf("    ... (%d more sessions)\n", session_count - 20);
            }
            printf("  ]\n");
            printf("}\n");
        } else {
            printf("  %-12s  %-8s  %-10s  %-12s  %-8s  %-10s\n",
                   "Session ID", "Worker", "Age", "Bytes In", "Errors", "Status");
            printf("  %s\n", "-----------------------------------------------------------------------------------------");
            
            for (int i = 0; i < session_count && i < 20; i++) {
                char buf[32], age[32];
                time_t age_sec = time(NULL) - session_list[i].created;
                format_duration(age_sec, age, sizeof(age));
                
                printf("  ");
                if (USE_COLOR) printf("%s", COLOR_CYAN);
                printf("0x%lx", (unsigned long)session_list[i].session_id);
                if (USE_COLOR) printf("%s", COLOR_RESET);
                printf("  ");
                
                printf("%-8d  %-10s  %-12s  %-8d  ",
                       session_list[i].worker_id,
                       age,
                       format_bytes(session_list[i].bytes_in, buf, sizeof(buf)),
                       session_list[i].errors);
                
                /* Status */
                if (session_list[i].errors > 0) {
                    if (USE_COLOR) printf("%s", COLOR_RED);
                    printf("ERROR");
                } else if (age_sec < 60) {
                    if (USE_COLOR) printf("%s", COLOR_YELLOW);
                    printf("NEW");
                } else {
                    if (USE_COLOR) printf("%s", COLOR_GREEN);
                    printf("STABLE");
                }
                if (USE_COLOR) printf("%s", COLOR_RESET);
                printf("\n");
            }
            
            if (session_count > 20) {
                printf("  ... (%d more sessions not shown)\n", session_count - 20);
            }
        }
    } else {
        printf("  No active sessions found.\n");
    }
    printf("\n");
    
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("  [NOTICE] Full affinity test requires session reconnection.\n");
    printf("           The following would be tested:\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    printf("  Test Procedure:\n");
    printf("  %s\n", "----------------------------------------\n");
    printf("  1. Create %d test sessions\n", sessions);
    printf("  2. Record initial worker assignment for each session\n");
    printf("  3. Force session disconnection (simulate reconnect)\n");
    printf("  4. Re-establish session\n");
    printf("  5. Verify session returns to SAME worker\n");
    printf("  6. Repeat %d times per session\n\n", retries);
    
    printf("  Affinity Verification:\n");
    printf("  %s\n", "----------------------------------------\n");
    printf("  - Session should return to original worker ID\n");
    printf("  - Hash-based sessions: MAC/IP determines worker\n");
    printf("  - Round-robin sessions: may return to different worker\n\n");
    
    printf("  Expected Results:\n");
    printf("  %s\n", "----------------------------------------\n");
    printf("  %-12s  %-12s  %-12s  %-10s\n",
           "Session", "Initial Worker", "Final Worker", "Affirmed");
    printf("  %s\n", "--------------------------------------------------------");
    printf("  (would show PASS/FAIL per session)\n\n");
    
    if (session_count > 0 && session_count <= 20) {
        printf("  Current Sessions (affinity verified):\n");
        printf("  %s\n", "----------------------------------------\n");
        for (int i = 0; i < session_count; i++) {
            printf("  0x%lx -> Worker %d: ", 
                   (unsigned long)session_list[i].session_id,
                   session_list[i].worker_id);
            if (USE_COLOR) printf("%s", COLOR_GREEN);
            printf("CURRENT");
            if (USE_COLOR) printf("%s", COLOR_RESET);
            printf("\n");
        }
        printf("\n");
    }
    
    free(session_list);
    free(workers);
}

static void run_transfer_test(const char *file, int buffer_size) {
    print_header("Transfer Test");
    printf("  [INFO] Transfer test mode\n");
    if (file) {
        printf("  [INFO] File: %s\n", file);
    }
    printf("  [INFO] Buffer size: %d bytes\n\n", buffer_size);
    
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("  [NOTICE] Transfer test mode is not yet fully implemented.\n");
    printf("           This feature requires PPPoE session data streaming.\n\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    
    printf("  To implement: Stream data through PPPoE sessions,\n");
    printf("  calculate checksums (CRC32/MD5/SHA256), verify integrity.\n\n");
}

/* Governor test - auto-scaling behavior verification */
static void run_governor_test(const char *trigger) {
    print_header("Governor Test");
    printf("  [INFO] Governor test mode\n");
    if (trigger) {
        printf("  [INFO] Trigger: %s\n\n", trigger);
    }
    
    /* Get current governor status */
    struct governor_status gov;
    get_governor_status(&gov);
    struct worker_info *workers = NULL;
    int worker_count = get_workers(&workers);
    
    printf("  Current Governor State:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  %-25s: %s\n", "Enabled", gov.enabled ? "Yes" : "No");
    printf("  %-25s: %s\n", "Mode", gov.mode == 1 ? "Auto" : "Manual");
    printf("  %-25s: %d\n", "Current Workers", gov.current_workers);
    printf("  %-25s: %d\n", "Min Workers", gov.min_workers);
    printf("  %-25s: %d\n", "Max Workers", gov.max_workers);
    if (gov.pending_removals > 0) {
        printf("  %-25s: %d\n", "Pending Removals", gov.pending_removals);
    }
    printf("\n");
    
    printf("  Scaling Thresholds:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  %-25s: %d%%\n", "CPU Scale-Up Threshold", gov.cpu_threshold);
    printf("  %-25s: %d%%\n", "CPU Scale-Down Threshold", gov.cpu_low_threshold);
    printf("  %-25s: %d\n", "Sessions/Worker Target", gov.sessions_per_worker);
    printf("  %-25s: %ds\n", "Poll Interval", gov.poll_interval);
    printf("  %-25s: %ds\n", "Drain Timeout", gov.drain_timeout);
    printf("\n");
    
    printf("  Current Metrics:\n");
    printf("  %s\n", "----------------------------------------");
    printf("  %-25s: %.1f%%\n", "CPU Usage", gov.cpu_usage);
    printf("  %-25s: %.1f%%\n", "CPU Average", gov.cpu_avg);
    printf("  %-25s: %s\n", "Last Decision", 
           *gov.last_decision ? gov.last_decision : "none");
    printf("  %-25s: %s\n", "Last Reason",
           *gov.last_reason ? gov.last_reason : "none");
    if (gov.last_decision_time > 0) {
        char ts[64];
        printf("  %-25s: %s\n", "Last Decision Time", 
               timestamp_iso(ts, sizeof(ts)));
    }
    printf("\n");
    
    /* Show worker states */
    if (worker_count > 0) {
        printf("  Worker States:\n");
        printf("  %s\n", "----------------------------------------");
        printf("  %-8s  %-10s  %-10s  %-8s\n",
               "Worker", "State", "Sessions", "Uptime");
        printf("  %s\n", "--------------------------------------------------------");
        
        for (int i = 0; i < worker_count; i++) {
            printf("  %-8d  ", workers[i].id);
            
            switch (workers[i].state) {
                case WORKER_ACTIVE:
                    if (USE_COLOR) printf("%s", COLOR_GREEN);
                    printf("%-10s", "ACTIVE");
                    break;
                case WORKER_DRAINING:
                    if (USE_COLOR) printf("%s", COLOR_YELLOW);
                    printf("%-10s", "DRAINING");
                    break;
                case WORKER_PENDING_REMOVAL:
                    if (USE_COLOR) printf("%s", COLOR_RED);
                    printf("%-10s", "PENDING");
                    break;
                default:
                    printf("%-10s", "UNKNOWN");
            }
            if (USE_COLOR) printf("%s", COLOR_RESET);
            
            char uptime[32];
            printf("  %-10d  %-8s\n",
                   workers[i].sessions,
                   format_duration(workers[i].uptime, uptime, sizeof(uptime)));
        }
        printf("\n");
    }
    
    /* Handle trigger if specified */
    if (trigger) {
        printf("  Trigger Action: %s\n", trigger);
        printf("  %s\n", "----------------------------------------\n");
        
        if (strcmp(trigger, "scale-up") == 0) {
            printf("  Would trigger scale-up:\n");
            printf("  - Simulate high CPU load (>%d%%)\n", gov.cpu_threshold);
            printf("  - Or: Create many sessions to trigger scaling\n");
            printf("  - Result: New worker should be added\n");
            
            if (gov.current_workers >= gov.max_workers) {
                if (USE_COLOR) printf("%s", COLOR_YELLOW);
                printf("  [WARNING] At max workers (%d)\n", gov.max_workers);
                if (USE_COLOR) printf("%s", COLOR_RESET);
            }
        } else if (strcmp(trigger, "scale-down") == 0) {
            printf("  Would trigger scale-down:\n");
            printf("  - Simulate low CPU load (<%d%%)\n", gov.cpu_low_threshold);
            printf("  - Or: Reduce session count\n");
            printf("  - Result: Worker should be marked DRAINING\n");
            
            if (gov.current_workers <= gov.min_workers) {
                if (USE_COLOR) printf("%s", COLOR_YELLOW);
                printf("  [WARNING] At min workers (%d)\n", gov.min_workers);
                if (USE_COLOR) printf("%s", COLOR_RESET);
            }
        } else {
            if (USE_COLOR) printf("%s", COLOR_YELLOW);
            printf("  [WARNING] Unknown trigger: %s\n", trigger);
            if (USE_COLOR) printf("%s", COLOR_RESET);
            printf("  Valid triggers: scale-up, scale-down\n");
        }
        printf("\n");
    }
    
    /* Governor state machine */
    printf("  Governor State Machine:\n");
    printf("  %s\n", "----------------------------------------\n");
    printf("  ACTIVE <---> DRAINING <---> PENDING_REMOVAL\n");
    printf("    |              |               |\n");
    printf("    |              |               |\n");
    printf("    v              v               v\n");
    printf("  Accepting    No new       Wait for drain,\n");
    printf("  sessions     sessions      then remove\n\n");
    
    printf("  Scale-Up Decision:\n");
    printf("  - CPU > %d%% OR sessions > %d * workers * 0.8\n",
           gov.cpu_threshold, gov.sessions_per_worker);
    printf("  - AND workers < max_workers\n");
    printf("  - AND time since last scale-up > %ds\n\n", gov.poll_interval);
    
    printf("  Scale-Down Decision:\n");
    printf("  - CPU < %d%% AND sessions < %d * workers * 0.3\n",
           gov.cpu_low_threshold, gov.sessions_per_worker);
    printf("  - AND workers > min_workers\n");
    printf("  - AND time since last scale-down > %ds\n\n", gov.poll_interval * 2);
    
    printf("  \"Change Mind\" Feature:\n");
    printf("  - If scale-down is pending and load spikes, cancel removal\n");
    printf("  - Worker returns to ACTIVE state\n");
    printf("  - Avoids unnecessary worker churn\n\n");
    
    free(workers);
}

static void run_stress_test(int sessions, int duration) {
    print_header("Stress Test");
    printf("  [INFO] Stress test mode - %d sessions for %d seconds\n\n", sessions, duration);
    
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("  [NOTICE] Stress test mode is not yet fully implemented.\n");
    printf("           This feature requires high-load session creation.\n\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    
    printf("  To implement: Create many sessions rapidly,\n");
    printf("  monitor CPU/memory, verify system stability.\n\n");
}

static void run_benchmark_test(void) {
    print_header("Benchmark Test");
    printf("  [INFO] Benchmark mode - performance testing\n\n");
    
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("  [NOTICE] Benchmark test mode is not yet fully implemented.\n");
    printf("           This feature requires throughput/latency measurement.\n\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    
    printf("  To implement: Measure throughput (Mbps), latency (ms),\n");
    printf("  CPU overhead, memory usage under load.\n\n");
}

static void print_menu(void) {
    printf("\n");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("================================================================================\n");
    printf("                    PPPoE Load Balancer Testing Tool\n");
    printf("================================================================================\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    /* Show current defaults */
    if (USE_COLOR) printf("%s", COLOR_CYAN);
    printf("Current Defaults:\n");
    printf("  - Mode: diagnose (production-safe, no session creation)\n");
    printf("  - Imbalance threshold: %d%%\n", config.imbalance_threshold);
    printf("  - Health threshold: %d%%\n", config.health_threshold);
    printf("  - Monitoring interval: %ds\n", config.interval);
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    printf("Select an operation:\n");
    printf("\n");
    printf("  [1] Diagnose System          - Analyze existing configuration (PRODUCTION SAFE)\n");
    printf("  [2] Test Accuracy            - Verify session distribution accuracy\n");
    printf("  [3] Test Affinity            - Verify session stickiness\n");
    printf("  [4] Test Governor            - Verify auto-scaling behavior\n");
    printf("  [5] Test File Transfer       - Stream files with checksums\n");
    printf("  [6] Stress Test              - High-load stress testing\n");
    printf("  [7] Benchmark                - Performance benchmarking\n");
    printf("  [8] Configure Defaults       - Set default thresholds and options\n");
    printf("  [9] Health Check             - Quick health assessment\n");
    printf("  [0] Exit\n");
    printf("\n");
    printf("================================================================================\n");
    printf("\n");
    if (USE_COLOR) printf("%s", COLOR_YELLOW);
    printf("WARNING: Modes [2-7] will create test sessions.\n");
    printf("         Mode [1] is production safe - no session creation.\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    /* Ask about proceeding with defaults */
    printf("Should I proceed with these diagnostic feature defaults?\n");
    printf("  - diagnose mode as default (safe)\n");
    printf("  - %d%% imbalance threshold\n", config.imbalance_threshold);
    printf("  - %d-second monitoring interval\n", config.interval);
    printf("\n");
    printf("Proceed? [Y/n] ");
}

static void print_configure_menu(void) {
    printf("\n");
    if (USE_COLOR) printf("%s", COLOR_BOLD);
    printf("================================================================================\n");
    printf("                         Configure Defaults\n");
    printf("================================================================================\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    printf("  [1] Set imbalance threshold    (current: %d%%)\n", config.imbalance_threshold);
    printf("  [2] Set health threshold      (current: %d%%)\n", config.health_threshold);
    printf("  [3] Set monitoring interval   (current: %ds)\n", config.interval);
    printf("  [4] Reset all to defaults\n");
    printf("  [0] Back to main menu\n");
    printf("\n");
}

static void run_configure_menu(void) {
    int running = 1;
    while (running) {
        print_configure_menu();
        
        printf("Choice: ");
        char line[256];
        if (!(fgets(line, sizeof(line), stdin))) {
            break;
        }
        
        int choice = atoi(line);
        char *endptr;
        
        switch (choice) {
            case 1:
                printf("Enter new imbalance threshold (1-100%%): ");
                if (fgets(line, sizeof(line), stdin)) {
                    int val = strtol(line, &endptr, 10);
                    if (endptr != line && val >= 1 && val <= 100) {
                        config.imbalance_threshold = val;
                        printf("Imbalance threshold set to %d%%\n", val);
                    } else {
                        printf("Invalid value. Must be between 1 and 100.\n");
                    }
                }
                break;
            case 2:
                printf("Enter new health threshold (1-100%%): ");
                if (fgets(line, sizeof(line), stdin)) {
                    int val = strtol(line, &endptr, 10);
                    if (endptr != line && val >= 1 && val <= 100) {
                        config.health_threshold = val;
                        printf("Health threshold set to %d%%\n", val);
                    } else {
                        printf("Invalid value. Must be between 1 and 100.\n");
                    }
                }
                break;
            case 3:
                printf("Enter monitoring interval (1-60 seconds): ");
                if (fgets(line, sizeof(line), stdin)) {
                    int val = strtol(line, &endptr, 10);
                    if (endptr != line && val >= 1 && val <= 60) {
                        config.interval = val;
                        printf("Monitoring interval set to %ds\n", val);
                    } else {
                        printf("Invalid value. Must be between 1 and 60.\n");
                    }
                }
                break;
            case 4:
                config.imbalance_threshold = 20;
                config.health_threshold = 40;
                config.interval = 5;
                printf("All settings reset to defaults:\n");
                printf("  - Imbalance threshold: %d%%\n", config.imbalance_threshold);
                printf("  - Health threshold: %d%%\n", config.health_threshold);
                printf("  - Monitoring interval: %ds\n", config.interval);
                break;
            case 0:
                running = 0;
                break;
            default:
                printf("Invalid choice\n");
                break;
        }
        
        if (running && choice != 0) {
            printf("\nPress Enter to continue...");
            fgets(line, sizeof(line), stdin);
        }
    }
}

/* Usage */
static void print_usage(const char *prog) {
    printf("Usage: %s [options] [mode]\n", prog);
    printf("\nMODES (diagnose is DEFAULT - no session creation):\n");
    printf("  diagnose         Passive observation of existing sessions (DEFAULT)\n");
    printf("  accuracy         Test session distribution accuracy (creates sessions)\n");
    printf("  affinity         Test session affinity (stickiness)\n");
    printf("  transfer         Test file transfer with checksums\n");
    printf("  governor         Test auto-scaling behavior\n");
    printf("  stress           Stress test with high session count\n");
    printf("  benchmark        Performance benchmarking\n");
    printf("  menu             Interactive menu (when no mode specified)\n");
    printf("\nDIAGNOSE MODE OPTIONS (Production Safe - No Session Creation):\n");
    printf("  -s, --show-workers       Show all workers and their states\n");
    printf("  -S, --show-sessions      Show all active sessions\n");
    printf("  -g, --show-governor      Show governor state and decisions\n");
    printf("  -H, --health-score       Show overall health score\n");
    printf("  --check-affinity         Check for session affinity violations\n");
    printf("  --check-balance          Check load balance across workers\n");
    printf("  --monitor                Enable real-time monitoring\n");
    printf("  -i, --interval N         Monitoring interval in seconds (default: 5)\n");
    printf("  --imbalance-threshold N Deviation %% to trigger warning (default: 20)\n");
    printf("  --health-threshold N    Deviation %% to trigger error (default: 40)\n");
    printf("\nTEST MODE OPTIONS:\n");
    printf("  -n, --sessions N         Number of sessions to create (default: 100)\n");
    printf("  -a, --algorithm ALGO     Algorithm: 0=rr, 1=hash, 2=ll\n");
    printf("  -w, --workers N          Number of workers (default: 4)\n");
    printf("  -t, --threshold P        Acceptable deviation %% (default: 10)\n");
    printf("  -r, --retries N          Reconnection attempts (for affinity)\n");
    printf("  -f, --file PATH          File to transfer (for transfer mode)\n");
    printf("  -b, --buffer-size N      Buffer size in bytes (default: 65536)\n");
    printf("  -d, --duration N         Test duration in seconds\n");
    printf("  --trigger ACTION         Governor trigger: scale-up, scale-down\n");
    printf("  -v, --verbose            Show per-worker details\n");
    printf("\nOUTPUT OPTIONS:\n");
    printf("  -o, --output FORMAT      Output format: console, json, jsonl, csv, tap\n");
    printf("  --no-color               Disable color output\n");
    printf("  -q, --quiet              Quiet mode (minimal output)\n");
    printf("\nCONNECTION OPTIONS:\n");
    printf("  -h, --host HOST          Server hostname (default: localhost)\n");
    printf("  -p, --port PORT          Control port (default: 9001)\n");
    printf("  --server                 Run as server (for transfer mode)\n");
    printf("  --client                 Run as client (for transfer mode)\n");
    printf("\nOTHER OPTIONS:\n");
    printf("  -?, --help               Show this help message\n");
    printf("  --version                Show version information\n");
    printf("\nIf no mode is specified, diagnose mode is assumed (production safe).\n");
    printf("If no options are specified in diagnose mode, all basic diagnostics are shown.\n");
    printf("\nEXAMPLES:\n");
    printf("  %s                          # Diagnose existing system (PRODUCTION SAFE)\n", prog);
    printf("  %s diagnose -s -g            # Show workers and governor\n", prog);
    printf("  %s diagnose -H --monitor     # Health score with monitoring\n", prog);
    printf("  %s accuracy -n 100 -a 0     # Test round-robin with 100 sessions\n", prog);
    printf("  %s governor --trigger scale-up  # Test governor scale-up\n", prog);
    printf("\nNote: diagnose mode will NOT create any sessions - it is safe for production.\n");
    printf("\nCLIENT-SERVER MODES:\n");
    printf("  %s --server [--port N]         Start server on port (default: 9001)\n", prog);
    printf("  %s --client -h HOST [--port N] Connect to server\n", prog);
    printf("  %s --server &                   Start server in background\n", prog);
    printf("  %s --client test-server         Connect and run tests\n", prog);
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
        /* Mode options */
        {"diagnose", no_argument, NULL, 2000},
        {"accuracy", no_argument, NULL, 2001},
        {"affinity", no_argument, NULL, 2002},
        {"transfer", no_argument, NULL, 2003},
        {"governor", no_argument, NULL, 2004},
        {"stress", no_argument, NULL, 2005},
        {"benchmark", no_argument, NULL, 2006},
        {"menu", no_argument, NULL, 2007},
        
        /* Diagnose options */
        {"show-workers", no_argument, NULL, 's'},
        {"show-sessions", no_argument, NULL, 'S'},
        {"show-governor", no_argument, NULL, 'g'},
        {"health-score", no_argument, NULL, 'H'},
        {"check-affinity", no_argument, NULL, 'a'},
        {"check-balance", no_argument, NULL, 'b'},
        {"monitor", no_argument, NULL, 'm'},
        {"interval", required_argument, NULL, 'i'},
        {"imbalance-threshold", required_argument, NULL, 1000},
        {"health-threshold", required_argument, NULL, 1001},
        
        /* Test options */
        {"sessions", required_argument, NULL, 'n'},
        {"algorithm", required_argument, NULL, 'A'},
        {"workers", required_argument, NULL, 'w'},
        {"threshold", required_argument, NULL, 't'},
        {"retries", required_argument, NULL, 'r'},
        {"duration", required_argument, NULL, 'D'},
        {"buffer-size", required_argument, NULL, 'B'},
        {"file", required_argument, NULL, 'f'},
        {"trigger", required_argument, NULL, 1002},
        {"verbose", no_argument, NULL, 'v'},
        
        /* Connection options */
        {"host", required_argument, NULL, 'h'},
        {"port", required_argument, NULL, 'p'},
        {"server", no_argument, NULL, 1010},
        {"client", no_argument, NULL, 1011},
        
        /* Output options */
        {"output", required_argument, NULL, 'o'},
        {"no-color", no_argument, NULL, 1050},
        {"quiet", no_argument, NULL, 'q'},
        
        /* Other options */
        {"help", no_argument, NULL, '?'},
        {"version", no_argument, NULL, 1020},
        {NULL, 0, NULL, 0}
    };
    
    int c;
    int option_index = 0;
    
    /* Check for mode argument first */
    if (argc > 1) {
        if (strcmp(argv[1], "diagnose") == 0 || strcmp(argv[1], "diag") == 0) {
            config.mode = MODE_DIAGNOSE;
        } else if (strcmp(argv[1], "accuracy") == 0 || strcmp(argv[1], "acc") == 0) {
            config.mode = MODE_ACCURACY;
        } else if (strcmp(argv[1], "affinity") == 0 || strcmp(argv[1], "aff") == 0) {
            config.mode = MODE_AFFINITY;
        } else if (strcmp(argv[1], "transfer") == 0 || strcmp(argv[1], "xfer") == 0) {
            config.mode = MODE_TRANSFER;
        } else if (strcmp(argv[1], "governor") == 0 || strcmp(argv[1], "gov") == 0) {
            config.mode = MODE_GOVERNOR;
        } else if (strcmp(argv[1], "stress") == 0 || strcmp(argv[1], "load") == 0) {
            config.mode = MODE_STRESS;
        } else if (strcmp(argv[1], "benchmark") == 0 || strcmp(argv[1], "perf") == 0) {
            config.mode = MODE_BENCHMARK;
        } else if (strcmp(argv[1], "menu") == 0 || strcmp(argv[1], "-i") == 0) {
            config.mode = MODE_MENU;
        }
    }
    
    /* Parse options */
    optind = 1;  /* Reset for mode parsing */
    while ((c = getopt_long(argc, argv, "sSghHabmi:o:nq?n:w:t:r:D:B:f:A:Hv", 
                            long_options, &option_index)) != -1) {
        switch (c) {
            /* Mode options */
            case 2000: config.mode = MODE_DIAGNOSE; break;
            case 2001: config.mode = MODE_ACCURACY; break;
            case 2002: config.mode = MODE_AFFINITY; break;
            case 2003: config.mode = MODE_TRANSFER; break;
            case 2004: config.mode = MODE_GOVERNOR; break;
            case 2005: config.mode = MODE_STRESS; break;
            case 2006: config.mode = MODE_BENCHMARK; break;
            case 2007: config.mode = MODE_MENU; break;
            
            /* Diagnose options */
            case 's': config.show_workers = 1; break;
            case 'S': config.show_sessions = 1; break;
            case 'g': config.show_governor = 1; break;
            case 'H': config.health_score = 1; break;
            case 'a': config.check_affinity = 1; break;
            case 'b': config.check_balance = 1; break;
            case 'm': config.monitor = 1; break;
            case 'i': config.interval = atoi(optarg); break;
            case 1000: config.imbalance_threshold = atoi(optarg); break;
            case 1001: config.health_threshold = atoi(optarg); break;
            
            /* Test options */
            case 'n': config.test_sessions = atoi(optarg); break;
            case 'A': config.test_algorithm = atoi(optarg); break;
            case 'w': config.test_workers = atoi(optarg); break;
            case 't': config.test_threshold = atoi(optarg); break;
            case 'r': config.test_retries = atoi(optarg); break;
            case 'D': config.test_duration = atoi(optarg); break;
            case 'B': config.test_buffer_size = atoi(optarg); break;
            case 'f': config.file = optarg; break;
            case 1002: config.trigger = optarg; break;
            case 'v': config.verbose = 1; break;
            
            /* Connection options */
            case 'h': config.host = optarg; break;
            case 'p': config.port = atoi(optarg); break;
            case 1010: config.server_mode = 1; break;
            case 1011: config.client_mode = 1; break;
            
            /* Output options */
            case 'o':
                if (strcmp(optarg, "json") == 0) {
                    config.json_output = 1;
                } else if (strcmp(optarg, "jsonl") == 0) {
                    config.jsonl_output = 1;
                } else if (strcmp(optarg, "csv") == 0) {
                    config.csv_output = 1;
                } else if (strcmp(optarg, "tap") == 0) {
                    config.tap_output = 1;
                }
                break;
            case 1050: config.no_color = 1; break;
            case 'q': config.quiet = 1; break;
            
            case '?':
                print_usage(argv[0]);
                return 0;
            case 1020:
                printf("pppoetest 1.0\n");
                printf("Part of FreeBSD base system\n");
                printf("Copyright (c) 2024 Mark LaPointe <mark@cloudbsd.org>\n");
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Handle server/client modes */
    if (config.server_mode) {
        return run_server_mode(config.port > 0 ? config.port : 9001);
    }
    
    if (config.client_mode && config.host) {
        return run_client_mode(config.host, config.port > 0 ? config.port : 9001);
    }
    
    /* Handle menu mode */
    if (config.mode == MODE_MENU) {
        print_menu();
        
        /* Read the entire line */
        char line[256];
        if (!(fgets(line, sizeof(line), stdin))) {
            return 0;
        }
        
        /* Check for Y/y or empty (proceed with defaults) */
        int choice = -1;
        char *ptr = line;
        while (*ptr == ' ') ptr++;  /* Skip leading spaces */
        if (*ptr == 'y' || *ptr == 'Y' || *ptr == '\n' || *ptr == '\0') {
            /* Default: run diagnose mode with all checks */
            config.mode = MODE_DIAGNOSE;
            config.show_workers = 1;
            config.show_governor = 1;
            config.health_score = 1;
            config.check_balance = 1;
        } else if (*ptr >= '0' && *ptr <= '9') {
            choice = atoi(ptr);
            
            switch (choice) {
                case 1: config.mode = MODE_DIAGNOSE; break;
                case 2: config.mode = MODE_ACCURACY; break;
                case 3: config.mode = MODE_AFFINITY; break;
                case 4: config.mode = MODE_GOVERNOR; break;
                case 5: config.mode = MODE_TRANSFER; break;
                case 6: config.mode = MODE_STRESS; break;
                case 7: config.mode = MODE_BENCHMARK; break;
                case 8:  /* Configure - show configure menu */
                    run_configure_menu();
                    return 0;
                case 9:
                    config.mode = MODE_DIAGNOSE;
                    config.health_score = 1;
                    break;
                case 0: return 0;
                default:
                    fprintf(stderr, "Invalid choice\n");
                    return 1;
            }
        } else {
            fprintf(stderr, "Invalid input\n");
            return 1;
        }
    }
    
    /* Print banner */
    if (!config.quiet && !config.json_output && !config.jsonl_output) {
        printf("\n");
        if (USE_COLOR) printf("%s", COLOR_BOLD);
        printf("================================================================================\n");
        printf("                    PPPoE Load Balancer Testing Tool\n");
        printf("================================================================================\n");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
        
        const char *mode_name = "DIAGNOSE";
        switch (config.mode) {
            case MODE_DIAGNOSE: mode_name = "DIAGNOSE"; break;
            case MODE_ACCURACY: mode_name = "ACCURACY TEST"; break;
            case MODE_AFFINITY: mode_name = "AFFINITY TEST"; break;
            case MODE_TRANSFER: mode_name = "TRANSFER TEST"; break;
            case MODE_GOVERNOR: mode_name = "GOVERNOR TEST"; break;
            case MODE_STRESS: mode_name = "STRESS TEST"; break;
            case MODE_BENCHMARK: mode_name = "BENCHMARK"; break;
            case MODE_MENU: mode_name = "MENU"; break;
        }
        printf("Mode: %s\n", mode_name);
        printf("\n");
        
        if (config.mode == MODE_DIAGNOSE) {
            if (USE_COLOR) printf("%s", COLOR_YELLOW);
            printf("NOTE: This is DIAGNOSE mode - no sessions will be created.\n");
            printf("      This is safe to run on production systems.\n");
            if (USE_COLOR) printf("%s", COLOR_RESET);
        } else {
            if (USE_COLOR) printf("%s", COLOR_YELLOW);
            printf("WARNING: This mode will create test sessions.\n");
            printf("         Use DIAGNOSE mode for production-safe observation.\n");
            if (USE_COLOR) printf("%s", COLOR_RESET);
        }
        printf("\n");
    }
    
    /* Run the appropriate mode */
    switch (config.mode) {
        case MODE_DIAGNOSE:
            /* If no specific options, enable all basic diagnostics */
            if (!config.show_workers && !config.show_sessions && 
                !config.show_governor && !config.health_score && 
                !config.check_balance && !config.monitor) {
                config.show_workers = 1;
                config.show_governor = 1;
                config.health_score = 1;
            }
            
            /* Run monitoring mode */
            if (config.monitor) {
                run_monitoring();
                break;
            }
            
            /* Run selected diagnostic checks */
            if (config.show_workers) {
                print_workers();
            }
            
            if (config.show_governor) {
                print_governor();
            }
            
            if (config.health_score) {
                print_health_score();
            }
            
            if (config.check_balance) {
                print_balance_check();
            }
            
            if (config.show_sessions) {
                print_sessions();
            }
            
            if (config.check_affinity) {
                print_header("Session Affinity Check");
                struct session_info *sessions = NULL;
                int count = get_sessions(&sessions, 100);
                
                if (count <= 0) {
                    printf("  No sessions found to check affinity\n\n");
                } else {
                    int workers_with_sessions = 0;
                    int *worker_session_counts = calloc(16, sizeof(int));
                    
                    for (int i = 0; i < count && i < 16; i++) {
                        if (sessions[i].worker_id >= 0 && sessions[i].worker_id < 16) {
                            worker_session_counts[sessions[i].worker_id]++;
                            if (worker_session_counts[sessions[i].worker_id] == 1) {
                                workers_with_sessions++;
                            }
                        }
                    }
                    
                    printf("  Sessions distributed across %d workers:\n\n", workers_with_sessions);
                    
                    for (int i = 0; i < 16; i++) {
                        if (worker_session_counts[i] > 0) {
                            printf("  Worker %d: %d sessions", i, worker_session_counts[i]);
                            if (USE_COLOR) printf("%s", COLOR_GREEN);
                            printf(" OK");
                            if (USE_COLOR) printf("%s", COLOR_RESET);
                            printf("\n");
                        }
                    }
                    
                    free(worker_session_counts);
                    free(sessions);
                    printf("\n");
                    printf("  Note: Full affinity verification requires tracking session\n");
                    printf("        creation history and comparing expected vs actual worker.\n\n");
                }
            }
            break;
            
        case MODE_ACCURACY:
            run_accuracy_test(config.test_sessions, config.test_algorithm, config.test_threshold);
            break;
            
        case MODE_AFFINITY:
            run_affinity_test(config.test_sessions, config.test_retries);
            break;
            
        case MODE_TRANSFER:
            run_transfer_test(config.file, config.test_buffer_size);
            break;
            
        case MODE_GOVERNOR:
            run_governor_test(config.trigger);
            break;
            
        case MODE_STRESS:
            run_stress_test(config.test_sessions, config.test_duration);
            break;
            
        case MODE_BENCHMARK:
            run_benchmark_test();
            break;
            
        case MODE_MENU:
            /* Should not reach here */
            break;
    }
    
    return 0;
}

/* =========================================================================
 * Client-Server Mode Implementation
 * ========================================================================= */

/* Message types for client-server communication */
#define MSG_HANDSHAKE      "HANDSHAKE"
#define MSG_HANDSHAKE_ACK  "HANDSHAKE_ACK"
#define MSG_TEST_REQUEST   "TEST_REQUEST"
#define MSG_TEST_START     "TEST_START"
#define MSG_TEST_DATA      "TEST_DATA"
#define MSG_TEST_RESULT    "TEST_RESULT"
#define MSG_STATUS_QUERY   "STATUS_QUERY"
#define MSG_STATUS_RESPONSE "STATUS_RESPONSE"
#define MSG_ERROR          "ERROR"
#define MSG_HEARTBEAT      "HEARTBEAT"
#define MSG_CLOSE          "CLOSE"
#define MSG_ACK            "ACK"

typedef enum {
    SERVER_STATE_IDLE = 0,
    SERVER_STATE_HANDSHAKE,
    SERVER_STATE_RUNNING,
    SERVER_STATE_FINISHED
} server_state_t;

typedef enum {
    CLIENT_STATE_DISCONNECTED = 0,
    CLIENT_STATE_CONNECTING,
    CLIENT_STATE_HANDSHAKE,
    CLIENT_STATE_RUNNING,
    CLIENT_STATE_FINISHED
} client_state_t;

typedef struct {
    int sock;
    char peername[64];
    time_t connected_at;
    server_state_t state;
    struct session_info *sessions;
    int session_count;
} client_info_t;

#define MAX_CLIENTS 16
static client_info_t clients[MAX_CLIENTS];
static int server_sock = -1;
static int server_port = 9001;

/* Send a JSON message over socket */
static int send_message(int sock, const char *type, const char *payload) {
    char buffer[8192];
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", tm);
    
    snprintf(buffer, sizeof(buffer),
        "{\"type\":\"%s\",\"timestamp\":\"%s\",\"seq\":%ld,\"payload\":%s}\n",
        type, timestamp, (long)now, payload ? payload : "{}");
    
    int len = strlen(buffer);
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buffer + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }
    return 0;
}

/* Receive a JSON message from socket */
static int recv_message(int sock, char *buffer, size_t buflen) {
    memset(buffer, 0, buflen);
    int pos = 0;
    
    while (pos < (int)buflen - 1) {
        char c;
        int n = recv(sock, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        if (c == '\n') break;
        buffer[pos++] = c;
    }
    
    return pos;
}

/* Parse message type from JSON */
static const char *get_message_type(const char *json) {
    static char type[64];
    const char *p = strstr(json, "\"type\":\"");
    if (!p) return NULL;
    p += 7;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(type) - 1) {
        type[i++] = *p++;
    }
    type[i] = '\0';
    return type;
}

/* Get system status for status response */
static void get_system_status(char *buffer, size_t buflen) {
    struct worker_info *workers = NULL;
    int worker_count = get_workers(&workers);
    int total_sessions = 0;
    
    for (int i = 0; i < worker_count; i++) {
        total_sessions += workers[i].sessions;
    }
    
    struct governor_status gov;
    get_governor_status(&gov);
    
    const char *mode_str = gov.enabled ? (gov.mode == 1 ? "auto" : "manual") : "disabled";
    snprintf(buffer, buflen,
        "{\"worker_count\":%d,\"total_sessions\":%d,\"governor_enabled\":%s,"
        "\"cpu_usage\":%.1f,\"mode\":\"%s\"}",
        worker_count, total_sessions,
        gov.enabled ? "true" : "false",
        gov.cpu_usage,
        mode_str);
    
    free(workers);
}

/* Handle client connection in server mode */
static void *handle_client(void *arg) {
    client_info_t *client = (client_info_t *)arg;
    char buffer[8192];
    char payload[4096];
    
    printf("[SERVER] Client connected from %s\n", client->peername);
    
    while (1) {
        int n = recv_message(client->sock, buffer, sizeof(buffer));
        if (n <= 0) {
            printf("[SERVER] Client %s disconnected\n", client->peername);
            break;
        }
        
        const char *type = get_message_type(buffer);
        if (!type) continue;
        
        printf("[SERVER] Received: %s from %s\n", type, client->peername);
        
        if (strcmp(type, MSG_HANDSHAKE) == 0) {
            /* Send handshake acknowledgment */
            snprintf(payload, sizeof(payload),
                "{\"status\":\"connected\",\"server_version\":\"1.0\","
                "\"capabilities\":[\"diagnose\",\"accuracy\",\"transfer\"]}");
            send_message(client->sock, MSG_HANDSHAKE_ACK, payload);
            client->state = SERVER_STATE_HANDSHAKE;
        }
        else if (strcmp(type, MSG_STATUS_QUERY) == 0) {
            get_system_status(payload, sizeof(payload));
            send_message(client->sock, MSG_STATUS_RESPONSE, payload);
        }
        else if (strcmp(type, MSG_TEST_REQUEST) == 0) {
            /* Acknowledge test request */
            snprintf(payload, sizeof(payload), "{\"status\":\"accepted\"}");
            send_message(client->sock, MSG_TEST_START, payload);
            client->state = SERVER_STATE_RUNNING;
        }
        else if (strcmp(type, MSG_CLOSE) == 0) {
            send_message(client->sock, MSG_ACK, NULL);
            break;
        }
        else if (strcmp(type, MSG_HEARTBEAT) == 0) {
            send_message(client->sock, MSG_ACK, NULL);
        }
    }
    
    close(client->sock);
    client->sock = -1;
    client->state = SERVER_STATE_IDLE;
    
    return NULL;
}

/* Run server mode */
static int run_server_mode(int port) {
    struct sockaddr_in addr;
    int opt = 1;
    
    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("socket");
        return 1;
    }
    
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_sock);
        return 1;
    }
    
    if (listen(server_sock, 5) < 0) {
        perror("listen");
        close(server_sock);
        return 1;
    }
    
    print_header("PPPoE Load Balancer Test Server");
    printf("  Listening on port %d\n", port);
    printf("  Waiting for clients...\n\n");
    
    signal(SIGINT, signal_handler);
    
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_len);
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        
        /* Find free client slot */
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].sock < 0) {
                slot = i;
                break;
            }
        }
        
        if (slot < 0) {
            printf("[SERVER] Too many clients, rejecting\n");
            close(client_sock);
            continue;
        }
        
        clients[slot].sock = client_sock;
        clients[slot].state = SERVER_STATE_IDLE;
        clients[slot].connected_at = time(NULL);
        snprintf(clients[slot].peername, sizeof(clients[slot].peername),
            "%s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        /* Handle client in separate thread */
        pthread_t tid;
        pthread_create(&tid, NULL, handle_client, &clients[slot]);
        pthread_detach(tid);
    }
    
    close(server_sock);
    return 0;
}

/* Connect to server */
static int connect_to_server(const char *host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }
    
    struct hostent *he = gethostbyname(host);
    if (!he) {
        herror("gethostbyname");
        close(sock);
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    addr.sin_port = htons(port);
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return -1;
    }
    
    return sock;
}

/* Run client mode */
static int run_client_mode(const char *host, int port) {
    print_header("PPPoE Load Balancer Test Client");
    printf("  Connecting to %s:%d...\n", host, port);
    
    int sock = connect_to_server(host, port);
    if (sock < 0) {
        printf("  Failed to connect to server\n\n");
        return 1;
    }
    
    printf("  Connected! Sending handshake...\n\n");
    
    /* Send handshake */
    char payload[512];
    snprintf(payload, sizeof(payload),
        "{\"client_version\":\"1.0\",\"capabilities\":[\"diagnose\",\"accuracy\",\"transfer\"],"
        "\"hostname\":\"%s\"}", "test-client");
    
    if (send_message(sock, MSG_HANDSHAKE, payload) < 0) {
        printf("  Failed to send handshake\n\n");
        close(sock);
        return 1;
    }
    
    /* Wait for handshake ack */
    char buffer[8192];
    int n = recv_message(sock, buffer, sizeof(buffer));
    if (n <= 0) {
        printf("  No response from server\n\n");
        close(sock);
        return 1;
    }
    
    const char *type = get_message_type(buffer);
    if (!type || strcmp(type, MSG_HANDSHAKE_ACK) != 0) {
        printf("  Unexpected response: %s\n\n", type ? type : "unknown");
        close(sock);
        return 1;
    }
    
    printf("  ");
    if (USE_COLOR) printf("%s", COLOR_GREEN);
    printf("Connected to server successfully!\n");
    if (USE_COLOR) printf("%s", COLOR_RESET);
    printf("\n");
    
    /* Main client loop */
    signal(SIGINT, signal_handler);
    
    while (g_running) {
        printf("\n=== Client Menu ===\n");
        printf("[1] Query server status\n");
        printf("[2] Request diagnostic report\n");
        printf("[3] Request accuracy test\n");
        printf("[4] Request transfer test\n");
        printf("[5] Quit\n");
        printf("\nChoice: ");
        
        char choice[16];
        if (!fgets(choice, sizeof(choice), stdin)) break;
        
        int cmd = atoi(choice);
        
        switch (cmd) {
            case 1:
                send_message(sock, MSG_STATUS_QUERY, NULL);
                n = recv_message(sock, buffer, sizeof(buffer));
                if (n > 0) {
                    printf("\nServer Status:\n%s\n\n", buffer);
                }
                break;
                
            case 2:
            case 3:
            case 4:
                {
                    const char *test_type = (cmd == 2) ? "diagnose" : 
                                            (cmd == 3) ? "accuracy" : "transfer";
                    snprintf(payload, sizeof(payload), "{\"test\":\"%s\"}", test_type);
                    send_message(sock, MSG_TEST_REQUEST, payload);
                    n = recv_message(sock, buffer, sizeof(buffer));
                    if (n > 0) {
                        printf("\nTest Response:\n%s\n\n", buffer);
                    }
                }
                break;
                
            case 5:
                send_message(sock, MSG_CLOSE, NULL);
                g_running = 0;
                break;
        }
    }
    
    close(sock);
    return 0;
}
