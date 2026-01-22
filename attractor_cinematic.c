#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <float.h>
#include <openacc.h>

// --- Configuration ---
#define WIDTH 1920
#define HEIGHT 1080
#define NUM_PARTICLES 2000000
#define DT 0.012f  
#define EXPOSURE 2.5f 

// --- Constants ---
#define MAX_COORD 80.0f

#define TYPE_AIZAWA 0
#define TYPE_THOMAS 1
#define TYPE_LORENZ 2
#define TYPE_HALVORSEN 3
#define TYPE_CHEN 4
#define NUM_TYPES 5

static const char* ATTRACTOR_NAMES[NUM_TYPES] = {
    "Aizawa", "Thomas", "Lorenz", "Halvorsen", "Chen"
};

// Attractor-specific framing multipliers (mutable for config override)
static float ATTRACTOR_BASE_MULTIPLIERS[NUM_TYPES] = {
    0.8f,   // TYPE_AIZAWA - tighter (range ±2)
    0.8f,   // TYPE_THOMAS - tighter (range ±2)
    2.5f,   // TYPE_LORENZ - looser (range ±20-30)
    1.2f,   // TYPE_HALVORSEN - moderate (range ±3-5)
    2.5f    // TYPE_CHEN - looser (range ±20-30)
};

// Configurable zoom parameters (mutable for config override)
static float cfg_zoom_oscillation = 0.0f;   // Disabled breathing effect
static float cfg_dynamic_adjustment = 0.0f; // Disabled velocity-based zoom
static float cfg_screen_fill_factor = 0.07f; // 5x zoom out (0.35/5)
static float cfg_min_zoom = 60.0f;          // Prevent extreme zoom-out
static float cfg_max_zoom = 2000.0f;        // Upper bound for tight zoom
static float cfg_initial_cam_scale = -1.0f; // Initial camera scale (-1 = use default 100)

#define TRANSITION_FRAMES 120              // Blend duration (~2 sec at 60fps)

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

typedef struct { float a, b, c, d, e, f; } Params;

// --- Config File Parser ---
void load_config(const char* filename) {
    FILE* f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Warning: Could not open config file '%s', using defaults\n", filename);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char key[64];
        float value;
        if (sscanf(line, " %63[^= ] = %f", key, &value) == 2) {
            // Per-attractor zoom multipliers
            if (strcmp(key, "aizawa") == 0) {
                ATTRACTOR_BASE_MULTIPLIERS[TYPE_AIZAWA] = value;
            } else if (strcmp(key, "thomas") == 0) {
                ATTRACTOR_BASE_MULTIPLIERS[TYPE_THOMAS] = value;
            } else if (strcmp(key, "lorenz") == 0) {
                ATTRACTOR_BASE_MULTIPLIERS[TYPE_LORENZ] = value;
            } else if (strcmp(key, "halvorsen") == 0) {
                ATTRACTOR_BASE_MULTIPLIERS[TYPE_HALVORSEN] = value;
            } else if (strcmp(key, "chen") == 0) {
                ATTRACTOR_BASE_MULTIPLIERS[TYPE_CHEN] = value;
            }
            // Global zoom parameters
            else if (strcmp(key, "screen_fill_factor") == 0) {
                cfg_screen_fill_factor = value;
            } else if (strcmp(key, "min_zoom") == 0) {
                cfg_min_zoom = value;
            } else if (strcmp(key, "max_zoom") == 0) {
                cfg_max_zoom = value;
            } else if (strcmp(key, "zoom_oscillation") == 0) {
                cfg_zoom_oscillation = value;
            } else if (strcmp(key, "dynamic_adjustment") == 0) {
                cfg_dynamic_adjustment = value;
            } else if (strcmp(key, "initial_cam_scale") == 0) {
                cfg_initial_cam_scale = value;
            }
        }
    }

    fclose(f);
    fprintf(stderr, "Loaded config from '%s'\n", filename);
    fprintf(stderr, "  Multipliers: aizawa=%.2f thomas=%.2f lorenz=%.2f halvorsen=%.2f chen=%.2f\n",
            ATTRACTOR_BASE_MULTIPLIERS[TYPE_AIZAWA],
            ATTRACTOR_BASE_MULTIPLIERS[TYPE_THOMAS],
            ATTRACTOR_BASE_MULTIPLIERS[TYPE_LORENZ],
            ATTRACTOR_BASE_MULTIPLIERS[TYPE_HALVORSEN],
            ATTRACTOR_BASE_MULTIPLIERS[TYPE_CHEN]);
    fprintf(stderr, "  screen_fill=%.3f min_zoom=%.1f max_zoom=%.1f\n",
            cfg_screen_fill_factor, cfg_min_zoom, cfg_max_zoom);
}

float *h_x, *h_y, *h_z;
float *h_vx, *h_vy, *h_vz; 
float *accum_buffer;
unsigned char *out_buffer;

// --- CPU Helper ---
float rand_range_cpu(float min, float max) {
    return min + ((float)rand() / RAND_MAX) * (max - min);
}

// --- GPU Helper: Heatmap ---
#pragma acc routine seq
void get_heatmap_color(float t, float *r, float *g, float *b) {
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    // Boosted saturation
    if (t < 0.2f) { *r = 0.0f; *g = t*5.0f; *b = 1.0f; } 
    else if (t < 0.5f) { *r = 0.0f; *g = 1.0f; *b = 1.0f - (t-0.2f)*3.3f; } 
    else if (t < 0.8f) { *r = (t-0.5f)*3.3f; *g = 1.0f; *b = 0.0f; } 
    else { *r = 1.0f; *g = 1.0f - (t-0.8f)*5.0f; *b = (t-0.8f)*5.0f; }
}

void log_attractor(FILE *logf, int mins, int secs, int type, Params p) {
    if (!logf) return;
    switch(type) {
        case TYPE_AIZAWA:
            fprintf(logf, "%02d:%02d %s a=%.3f b=%.3f c=%.3f d=%.3f e=%.3f f=%.3f\n",
                    mins, secs, ATTRACTOR_NAMES[type], p.a, p.b, p.c, p.d, p.e, p.f);
            break;
        case TYPE_THOMAS:
            fprintf(logf, "%02d:%02d %s b=%.4f\n", mins, secs, ATTRACTOR_NAMES[type], p.b);
            break;
        case TYPE_LORENZ:
            fprintf(logf, "%02d:%02d %s sigma=%.2f rho=%.2f beta=%.3f\n",
                    mins, secs, ATTRACTOR_NAMES[type], p.a, p.b, p.c);
            break;
        case TYPE_HALVORSEN:
            fprintf(logf, "%02d:%02d %s a=%.3f\n", mins, secs, ATTRACTOR_NAMES[type], p.a);
            break;
        case TYPE_CHEN:
            fprintf(logf, "%02d:%02d %s a=%.2f b=%.2f c=%.2f\n",
                    mins, secs, ATTRACTOR_NAMES[type], p.a, p.b, p.c);
            break;
    }
}

Params get_target_params(int type) {
    Params p = {0};
    switch(type) {
        case TYPE_AIZAWA:
            p.a=0.95f; p.b=0.7f; p.c=0.6f; p.d=3.5f; p.e=0.25f; p.f=0.1f;
            p.d += rand_range_cpu(-0.5f, 0.5f); 
            break;
        case TYPE_THOMAS:
            p.b = 0.19f + rand_range_cpu(-0.02f, 0.02f);
            break;
        case TYPE_LORENZ:
            p.a=10.0f; p.b=28.0f; p.c=2.66f;
            p.b += rand_range_cpu(-5.0f, 5.0f);
            break;
        case TYPE_HALVORSEN:
            p.a = 1.4f + rand_range_cpu(-0.2f, 0.2f);
            break;
        case TYPE_CHEN: 
            p.a = 40.0f; p.b = 3.0f; p.c = 28.0f;
            break;
    }
    return p;
}

int main(int argc, char *argv[]) {
    int fragments = 20;
    int frames_per_fragment = 300;

    int num_particles = NUM_PARTICLES;
    const char* config_file = NULL;
    int start_type = TYPE_AIZAWA;  // Default starting attractor

    int opt;
    while ((opt = getopt(argc, argv, "n:f:p:c:s:")) != -1) {
        switch (opt) {
            case 'n': fragments = atoi(optarg); break;
            case 'f': frames_per_fragment = atoi(optarg); break;
            case 'p': num_particles = atoi(optarg); break;
            case 'c': config_file = optarg; break;
            case 's': start_type = atoi(optarg) % NUM_TYPES; break;
        }
    }

    // Load config file if specified (before any rendering)
    if (config_file) {
        load_config(config_file);
    }

    // Open chapter log file
    FILE *log_file = fopen("chapters.txt", "w");
    if (!log_file) {
        fprintf(stderr, "Warning: Could not open chapters.txt for writing\n");
    }
    int framerate = 60;  // For timestamp calculation

    h_x = (float*)malloc(num_particles * sizeof(float));
    h_y = (float*)malloc(num_particles * sizeof(float));
    h_z = (float*)malloc(num_particles * sizeof(float));
    h_vx = (float*)malloc(num_particles * sizeof(float));
    h_vy = (float*)malloc(num_particles * sizeof(float));
    h_vz = (float*)malloc(num_particles * sizeof(float));
    accum_buffer = (float*)malloc(WIDTH * HEIGHT * 3 * sizeof(float));
    out_buffer = (unsigned char*)malloc(WIDTH * HEIGHT * 3 * sizeof(unsigned char));

    srand(time(NULL));

    // Initial Random Box
    for (int i = 0; i < num_particles; i++) {
        h_x[i] = rand_range_cpu(-5.0f, 5.0f);
        h_y[i] = rand_range_cpu(-5.0f, 5.0f);
        h_z[i] = rand_range_cpu(-5.0f, 5.0f);
    }

    #pragma acc enter data copyin(h_x[0:num_particles], h_y[0:num_particles], h_z[0:num_particles], \
                                  h_vx[0:num_particles], h_vy[0:num_particles], h_vz[0:num_particles]) \
                         create(accum_buffer[0:WIDTH*HEIGHT*3], out_buffer[0:WIDTH*HEIGHT*3])

    int current_type = start_type;
    Params cur_p = get_target_params(start_type);
    Params target_p = cur_p;

    // Log initial attractor
    log_attractor(log_file, 0, 0, current_type, cur_p);

    float cam_scale = (cfg_initial_cam_scale > 0) ? cfg_initial_cam_scale : 100.0f;
    float cam_cx = 0.0f, cam_cy = 0.0f;
    float smooth_max_spd = 1.0f;
    float smooth_base_multiplier = ATTRACTOR_BASE_MULTIPLIERS[start_type];

    // Attractor transition blending
    int previous_type = start_type;
    float transition_blend = 1.0f;  // 1.0 = fully current, 0.0 = fully previous

    int total_frames = fragments * frames_per_fragment;
    int algo_timer = 0;

    for (int frame = 0; frame < total_frames; frame++) {
        
        if (frame % frames_per_fragment == 0) {
            algo_timer++;
            if (algo_timer >= 6) {
                previous_type = current_type;  // Save old type for blending
                current_type = (current_type + 1) % NUM_TYPES;
                algo_timer = 0;
                transition_blend = 0.0f;       // Start blending from previous
                // Only get new random params when attractor TYPE changes
                target_p = get_target_params(current_type);

                // Log attractor type change with timestamp
                int total_seconds = frame / framerate;
                int mins = total_seconds / 60;
                int secs = total_seconds % 60;
                log_attractor(log_file, mins, secs, current_type, target_p);
            }
            // Removed: target_p = get_target_params() was causing jumps every fragment
        }

        // Smoothly transition base multiplier when attractor changes
        float target_base_multiplier = ATTRACTOR_BASE_MULTIPLIERS[current_type];
        smooth_base_multiplier += (target_base_multiplier - smooth_base_multiplier) * 0.02f;

        float lerp = 0.02f;
        cur_p.a += (target_p.a - cur_p.a)*lerp; cur_p.b += (target_p.b - cur_p.b)*lerp;
        cur_p.c += (target_p.c - cur_p.c)*lerp; cur_p.d += (target_p.d - cur_p.d)*lerp;
        cur_p.e += (target_p.e - cur_p.e)*lerp; cur_p.f += (target_p.f - cur_p.f)*lerp;

        // Progress attractor transition blend
        if (transition_blend < 1.0f) {
            transition_blend += 1.0f / TRANSITION_FRAMES;
            if (transition_blend > 1.0f) transition_blend = 1.0f;
        }

        #pragma acc parallel loop present(accum_buffer)
        for(int i=0; i<WIDTH*HEIGHT*3; i++) accum_buffer[i] = 0.0f;

        float theta = frame * 0.005f;
        float cos_t = cosf(theta);
        float sin_t = sinf(theta);

        // --- PHYSICS UPDATE ---
        #pragma acc parallel loop present(h_x, h_y, h_z, h_vx, h_vy, h_vz)
        for (int i = 0; i < num_particles; i++) {
            float x = h_x[i]; float y = h_y[i]; float z = h_z[i];

            // Compute velocity for CURRENT attractor
            float dx_cur=0, dy_cur=0, dz_cur=0;
            if (current_type == TYPE_AIZAWA) {
                dx_cur = (z - cur_p.b) * x - cur_p.d * y;
                dy_cur = cur_p.d * x + (z - cur_p.b) * y;
                dz_cur = cur_p.c + cur_p.a * z - (z*z*z)/3.0f - (x*x + y*y) * (1.0f + cur_p.e * z) + cur_p.f * z * x*x*x;
            } else if (current_type == TYPE_THOMAS) {
                dx_cur = sinf(y) - cur_p.b * x; dy_cur = sinf(z) - cur_p.b * y; dz_cur = sinf(x) - cur_p.b * z;
            } else if (current_type == TYPE_LORENZ) {
                dx_cur = cur_p.a * (y - x); dy_cur = x * (cur_p.b - z) - y; dz_cur = x * y - cur_p.c * z;
            } else if (current_type == TYPE_HALVORSEN) {
                dx_cur = -cur_p.a*x - 4*y - 4*z - y*y; dy_cur = -cur_p.a*y - 4*z - 4*x - z*z; dz_cur = -cur_p.a*z - 4*x - 4*y - x*x;
            } else if (current_type == TYPE_CHEN) {
                dx_cur = cur_p.a * (y - x); dy_cur = (cur_p.c - cur_p.a)*x - x*z + cur_p.c*y; dz_cur = x*y - cur_p.b*z;
            }

            // Compute velocity for PREVIOUS attractor (for blending)
            float dx_prev=0, dy_prev=0, dz_prev=0;
            if (previous_type == TYPE_AIZAWA) {
                dx_prev = (z - cur_p.b) * x - cur_p.d * y;
                dy_prev = cur_p.d * x + (z - cur_p.b) * y;
                dz_prev = cur_p.c + cur_p.a * z - (z*z*z)/3.0f - (x*x + y*y) * (1.0f + cur_p.e * z) + cur_p.f * z * x*x*x;
            } else if (previous_type == TYPE_THOMAS) {
                dx_prev = sinf(y) - cur_p.b * x; dy_prev = sinf(z) - cur_p.b * y; dz_prev = sinf(x) - cur_p.b * z;
            } else if (previous_type == TYPE_LORENZ) {
                dx_prev = cur_p.a * (y - x); dy_prev = x * (cur_p.b - z) - y; dz_prev = x * y - cur_p.c * z;
            } else if (previous_type == TYPE_HALVORSEN) {
                dx_prev = -cur_p.a*x - 4*y - 4*z - y*y; dy_prev = -cur_p.a*y - 4*z - 4*x - z*z; dz_prev = -cur_p.a*z - 4*x - 4*y - x*x;
            } else if (previous_type == TYPE_CHEN) {
                dx_prev = cur_p.a * (y - x); dy_prev = (cur_p.c - cur_p.a)*x - x*z + cur_p.c*y; dz_prev = x*y - cur_p.b*z;
            }

            // Blend velocities: lerp from previous to current
            float dx = dx_prev + (dx_cur - dx_prev) * transition_blend;
            float dy = dy_prev + (dy_cur - dy_prev) * transition_blend;
            float dz = dz_prev + (dz_cur - dz_prev) * transition_blend;

            x += dx*DT; y += dy*DT; z += dz*DT;

            if (fabs(x) > MAX_COORD || fabs(y) > MAX_COORD || fabs(z) > MAX_COORD || isnan(x)) {
                float hash = (float)((i * 1327) % 1000) / 1000.0f;
                x = (hash - 0.5f) * 4.0f; y = (hash - 0.5f) * 4.0f; z = (hash - 0.5f) * 4.0f;
                dx=0; dy=0; dz=0;
            }

            h_x[i] = x; h_y[i] = y; h_z[i] = z;
            h_vx[i] = dx; h_vy[i] = dy; h_vz[i] = dz;
        }

        // --- STATS (MEAN & MAD) ---
        int sample_stride = 100;
        int num_samples = num_particles / sample_stride;
        float sum_x = 0, sum_y = 0, max_spd = 0.0f;
        #pragma acc parallel loop present(h_x, h_y, h_z, h_vx, h_vy, h_vz) reduction(+:sum_x, sum_y) reduction(max:max_spd)
        for (int i = 0; i < num_particles; i+=sample_stride) {
            float x = h_x[i]; float z = h_z[i]; float y = h_y[i];
            float rx = x * cos_t - z * sin_t;
            float ry = y;
            sum_x += rx; sum_y += ry;
            float spd = sqrtf(h_vx[i]*h_vx[i] + h_vy[i]*h_vy[i] + h_vz[i]*h_vz[i]);
            if (spd > max_spd) max_spd = spd;
        }
        float center_x = sum_x / num_samples;
        float center_y = sum_y / num_samples;

        float sum_dist_x = 0, sum_dist_y = 0;
        #pragma acc parallel loop present(h_x, h_y, h_z) reduction(+:sum_dist_x, sum_dist_y)
        for (int i = 0; i < num_particles; i+=sample_stride) {
            float x = h_x[i]; float z = h_z[i]; float y = h_y[i];
            float rx = x * cos_t - z * sin_t;
            float ry = y;
            sum_dist_x += fabsf(rx - center_x);
            sum_dist_y += fabsf(ry - center_y);
        }
        float mean_dist_x = sum_dist_x / num_samples;
        float mean_dist_y = sum_dist_y / num_samples;

        // --- SINUSOIDAL ZOOM ANIMATION ---
        // Create breathing effect over each fragment duration
        int fragment_frame = frame % frames_per_fragment;
        float cycle_progress = (float)fragment_frame / (float)frames_per_fragment;
        float zoom_wave = sinf(cycle_progress * 2.0f * M_PI);  // -1 to +1
        float sinusoidal_factor = 1.0f + zoom_wave * cfg_zoom_oscillation;

        // --- DYNAMIC ADJUSTMENT ---
        // Adjust zoom based on particle velocity variance
        float velocity_ratio = max_spd / (smooth_max_spd + 0.001f);  // Avoid division by zero
        float dynamic_factor = 1.0f + (velocity_ratio - 1.0f) * cfg_dynamic_adjustment;
        if (dynamic_factor < 0.85f) dynamic_factor = 0.85f;
        if (dynamic_factor > 1.15f) dynamic_factor = 1.15f;

        // --- CINEMATIC ZOOM CALCULATION ---
        // Apply hybrid multiplier: base × dynamic × sinusoidal
        float combined_multiplier = smooth_base_multiplier * dynamic_factor * sinusoidal_factor;
        float target_w = mean_dist_x * combined_multiplier;
        float target_h = mean_dist_y * combined_multiplier;

        if (target_w < 1.0f) target_w = 1.0f;
        if (target_h < 1.0f) target_h = 1.0f;

        float scale_w = (WIDTH * cfg_screen_fill_factor) / target_w;
        float scale_h = (HEIGHT * cfg_screen_fill_factor) / target_h;
        float target_scale = (scale_w < scale_h) ? scale_w : scale_h;

        if (target_scale < cfg_min_zoom) target_scale = cfg_min_zoom;
        if (target_scale > cfg_max_zoom) target_scale = cfg_max_zoom;

        cam_scale += (target_scale - cam_scale) * 0.005f;
        cam_cx += (center_x - cam_cx) * 0.005f;
        cam_cy += (center_y - cam_cy) * 0.005f;

        if (max_spd < 1.0f) max_spd = 1.0f;
        smooth_max_spd += (max_spd - smooth_max_spd) * 0.005f;

        // --- RENDER ---
        #pragma acc parallel loop present(h_x, h_y, h_z, h_vx, h_vy, h_vz, accum_buffer)
        for (int i = 0; i < num_particles; i++) {
            float x = h_x[i]; float y = h_y[i]; float z = h_z[i];

            float rx = x * cos_t - z * sin_t;
            float rz = x * sin_t + z * cos_t;
            float ry = y;

            // Orthographic projection - direct scaling without perspective division
            // cam_scale now directly controls pixels per unit
            int px = (int)((rx - cam_cx) * cam_scale + WIDTH / 2);
            int py = (int)((ry - cam_cy) * cam_scale + HEIGHT / 2);

            if (px >= 0 && px < WIDTH && py >= 0 && py < HEIGHT) {
                float spd = sqrtf(h_vx[i]*h_vx[i] + h_vy[i]*h_vy[i] + h_vz[i]*h_vz[i]);
                float t = spd / smooth_max_spd;
                
                float r, g, b;
                get_heatmap_color(t, &r, &g, &b);

                // Simplified fade based on depth for visual interest only (not projection)
                float depth_fade = 1.0f / (1.0f + fabsf(rz) * 0.01f);  // Slight fade for far particles

                int idx = (py * WIDTH + px) * 3;
                #pragma acc atomic update
                accum_buffer[idx+0] += r * depth_fade;
                #pragma acc atomic update
                accum_buffer[idx+1] += g * depth_fade;
                #pragma acc atomic update
                accum_buffer[idx+2] += b * depth_fade;
            }
        }

        // --- TONE MAP ---
        #pragma acc parallel loop present(accum_buffer, out_buffer)
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            int idx = i * 3;
            float r = accum_buffer[idx+0];
            float g = accum_buffer[idx+1];
            float b = accum_buffer[idx+2];

            r = logf(1.0f + r * EXPOSURE) * 45.0f;
            g = logf(1.0f + g * EXPOSURE) * 45.0f;
            b = logf(1.0f + b * EXPOSURE) * 45.0f;

            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;

            out_buffer[idx+0] = (unsigned char)r;
            out_buffer[idx+1] = (unsigned char)g;
            out_buffer[idx+2] = (unsigned char)b;
        }

        #pragma acc update self(out_buffer[0:WIDTH*HEIGHT*3])
        fwrite(out_buffer, 1, WIDTH * HEIGHT * 3, stdout);
        if (frame % 60 == 0) {
            fprintf(stderr, "Fr %d | Type: %d->%d | Blend: %.2f | Scale: %.1f\r",
                    frame, previous_type, current_type, transition_blend, cam_scale);
        }
    }

    // Close chapter log file
    if (log_file) {
        fclose(log_file);
        fprintf(stderr, "\nChapter log written to chapters.txt\n");
    }

    free(h_x); free(h_y); free(h_z); free(accum_buffer); free(out_buffer);
    return 0;
}
