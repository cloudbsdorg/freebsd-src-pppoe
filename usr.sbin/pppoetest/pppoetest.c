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

/* Global flag for signal handling */
static volatile sig_atomic_t g_running = 1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* Configuration */
static struct {
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
} config = {
    .interval = 5,
    .imbalance_threshold = 20,
    .health_threshold = 40,
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

static void print_health_score(void) {
    int score = calculate_health_score();
    
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
    
    if (score >= 90) {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_GREEN);
        printf("Status: HEALTHY");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - System operating normally.\n\n");
    } else if (score >= 70) {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("Status: WARNING");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - Some metrics need attention.\n\n");
    } else {
        printf("  ");
        if (USE_COLOR) printf("%s", COLOR_RED);
        printf("Status: CRITICAL");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf(" - Immediate attention required.\n\n");
    }
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
        
        printf("\n  Updating in %d seconds...\n", config.interval);
        
        /* Sleep with interruptible sleep */
        struct timespec tspec = { .tv_sec = config.interval, .tv_nsec = 0 };
        while (g_running && nanosleep(&tspec, &tspec) < 0 && errno == EINTR)
            ;
    }
}

/* Usage */
static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("\nMODES (diagnose is DEFAULT - no session creation):\n");
    printf("  diagnose         Passive observation of existing sessions (DEFAULT)\n");
    printf("\nDIAGNOSE MODE OPTIONS (Production Safe - No Session Creation):\n");
    printf("  -s, --show-workers       Show all workers and their states\n");
    printf("  -g, --show-governor      Show governor state and decisions\n");
    printf("  -H, --health-score       Show overall health score\n");
    printf("  --check-balance          Check load balance across workers\n");
    printf("  --monitor                Enable real-time monitoring\n");
    printf("  -i, --interval N         Monitoring interval in seconds (default: 5)\n");
    printf("  --imbalance-threshold N Deviation %% to trigger warning (default: 20)\n");
    printf("\nOUTPUT OPTIONS:\n");
    printf("  -o, --output FORMAT      Output format: console, json\n");
    printf("  --no-color               Disable color output\n");
    printf("\nOTHER OPTIONS:\n");
    printf("  -h, --help               Show this help message\n");
    printf("\nIf no options are specified, all basic diagnostics are shown.\n");
    printf("\nNote: diagnose mode will NOT create any sessions - it is safe for production.\n");
}

int main(int argc, char **argv) {
    static struct option long_options[] = {
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
        {"output", required_argument, NULL, 'o'},
        {"no-color", no_argument, NULL, 'n'},
        {"quiet", no_argument, NULL, 'q'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    
    int c;
    int option_index = 0;
    
    /* Parse options */
    while ((c = getopt_long(argc, argv, "sSghHabmi:o:nqh", 
                            long_options, &option_index)) != -1) {
        switch (c) {
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
            case 'o':
                if (strcmp(optarg, "json") == 0) {
                    config.json_output = 1;
                } else if (strcmp(optarg, "jsonl") == 0) {
                    config.jsonl_output = 1;
                }
                break;
            case 'n': config.no_color = 1; break;
            case 'q': config.quiet = 1; break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    /* Print banner */
    if (!config.quiet && !config.json_output && !config.jsonl_output) {
        printf("\n");
        if (USE_COLOR) printf("%s", COLOR_BOLD);
        printf("================================================================================\n");
        printf("                    PPPoE Load Balancer Testing Tool\n");
        printf("================================================================================\n");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
        printf("Mode: DIAGNOSE (Production Safe)\n");
        printf("\n");
        if (USE_COLOR) printf("%s", COLOR_YELLOW);
        printf("NOTE: This is DIAGNOSE mode - no sessions will be created.\n");
        printf("      This is safe to run on production systems.\n");
        if (USE_COLOR) printf("%s", COLOR_RESET);
        printf("\n");
    }
    
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
        return 0;
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
    
    return 0;
}
