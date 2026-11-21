/* What VanaCore (in the game's 32-bit process) and VanaCore.Dlss.exe (64-bit, where DLSS runs) share: a block of
 * memory named VANA_DLSS_MAP<game's pid>, and events named the same plus ".go" (the game: a frame's inputs are
 * on their way) and ".ready" (the helper: its state changed). Both sides compile it (C, fixed-size fields, no
 * pointers), so it lays out the same in 32 and 64 bits.
 *
 * Under Wine and Proton (cpu_sync below) the fence isn't shared: the same hand-over, with CPU waits in its place.
 *
 * Each frame the game draws the scene's colour, depth and motion into textures it shares with the helper (NT
 * handles it duplicates into the helper's process), leaves them in the COMMON state, signals the shared fence
 * to `frame`, fills in the frame's fields and sets ".go"; its queue waits on the fence for frame + 1 before
 * reading the output. The helper's queue waits for `frame`, runs DLSS into the output, leaves it in COMMON and
 * signals frame + 1. A helper that stops answering is let go: the game signals frame + 1 itself.
 *
 * Frame generation (DLSS-FG) goes the same way at the frame's end, with ".fg" for its event and a fence of its own:
 * the finished frame (and the scene alone, before the game's interface: hudless) with the scene's depth and motion
 * (DLSS's inputs, still this frame's) make fg_count frames between the frame before and this one, into fg_output.
 *
 * DLSS 5 (neural rendering, NGX's feature 18: nvngx_dlssnr.dll beside the helper, not shipped with VanaCore) goes
 * before DLSS in the same frame: with `nr` set the game hands the colour over as R16G16B16A16_FLOAT (what it
 * takes), the helper runs it through DLSS 5 at the render size, with the same depth and motion, and DLSS reads its
 * picture in the colour's place. A helper NGX doesn't give it to (nr_available 0, nr_message why) runs DLSS alone.
 * With the game's effects kept out of it (nr_fx: [graphics.effects] dlss5_fx_out) the colour is the scene before the
 * game's effects were drawn into it, and `fx` their own picture (the finished colour less that): the helper adds fx onto
 * DLSS 5's picture before DLSS reads it (nr_fx_ok 1 once it can).
 *
 * AMD FSR (`upscaler` VANA_UPSCALER_FSR) takes DLSS's place on the same textures, fence and events: AMD's FSR API
 * (amd_fidelityfx_loader_dx12.dll, with amd_fidelityfx_upscaler_dx12.dll, beside the helper) upscales the colour
 * with the same depth, motion and jitter, FSR 4 where AMD's DLL runs it (a Radeon RX 7000 or 9000), else FSR 3.1.
 * Either upscaler is handed the game's masks of what isn't the static world (reactive, tc below): FSR's reactive and
 * transparency-and-composition masks, DLSS's bias-current-colour mask.
 * A helper serves the one upscaler it was started for: the game sets `quit` (and ".go") to end it and starts another
 * for the other. DLSS 5 is DLSS's alone (nr_available stays 0 with FSR).
 *
 * Frame generation is of one of two kinds (fg_kind, with the frame's textures: fg_generation), with either upscaler:
 * NVIDIA's (DLSS-FG, through NGX: a DLSS helper only) or AMD's (FSR frame generation: amd_fidelityfx_framegeneration_dx12.dll
 * beside the helper; FSR FG 4 where AMD's DLL runs it, a Radeon RX 9000 on Windows 11, else FSR FG 3.1; one frame between
 * two). AMD's goes without its swapchain: the helper configures it, dispatches its preparation on the scene's depth and
 * motion and its generation on the frame, into fg_output[0], and the game shows them as it does NVIDIA's. */
#ifndef VANA_DLSS_IPC_H
#define VANA_DLSS_IPC_H

#include <stdint.h>

#define VANA_DLSS_VERSION 16
#define VANA_DLSS_MAP L"Local\\VanaCoreDlss."

enum {
    VANA_UPSCALER_DLSS = 0, /* NVIDIA DLSS, through NGX */
    VANA_UPSCALER_FSR = 1,  /* AMD FSR, through AMD's FSR API */
};

enum {
    VANA_FG_DLSS = 0, /* NVIDIA's frame generation (DLSS-FG) */
    VANA_FG_FSR = 1,  /* AMD's (FSR frame generation) */
};

enum {
    VANA_DLSS_STARTING = 0,
    VANA_DLSS_READY = 1,       /* the upscaler runs on this GPU */
    VANA_DLSS_UNAVAILABLE = 2, /* it doesn't (DLSS: not an RTX card, an old driver; FSR: no DLL, no provider for this
                                * card): message says why */
    VANA_DLSS_FAILED = 3,      /* something went wrong: message says what */
};

typedef struct VanaDlssShared {
    uint32_t version;     /* VANA_DLSS_VERSION, the game's */
    uint32_t game_pid;
    uint32_t luid_low;    /* the game's adapter (DXGI LUID): the helper's device on the same one */
    int32_t luid_high;
    /* the helper's side */
    volatile int32_t state; /* VANA_DLSS_* */
    uint32_t helper_pid;
    char message[256];
    volatile uint64_t done; /* the last frame the helper handed to its queue (its fence signal) */
    /* the game's side: the textures, again when `generation` changes */
    uint32_t generation;
    uint64_t color, depth, motion, output, fence; /* NT handles, in the helper's process */
    uint32_t in_w, in_h;   /* the inputs' size: colour, depth (R32_FLOAT view z... the scene's depth, 0-1) and
                            * motion (R16G16_FLOAT, render pixels, toward where each pixel was the frame before) */
    uint32_t out_w, out_h; /* the output's (R8G8B8A8_UNORM, unordered access) */
    uint32_t color_format, output_format; /* DXGI formats */
    int32_t quality;      /* NVSDK_NGX_PerfQuality_Value: DLAA, quality, balanced, performance, ultra performance */
    int32_t depth_inverted;
    /* each frame */
    volatile uint64_t frame; /* the fence value the inputs are ready at; the output at frame + 1 */
    uint32_t render_w, render_h; /* the part of the inputs drawn this frame, from (0, 0) */
    float jitter_x, jitter_y;    /* the scene's offset this frame, in render pixels */
    float motion_scale_x, motion_scale_y;
    int32_t reset;               /* 1 after a cut (a new zone, the camera's jump) */
    float sharpness;
    /* frame generation: the helper's side */
    int32_t fg_available;         /* 1 when DLSS-FG runs on this GPU (RTX 40 or later); fg_message says why not */
    char fg_message[160];
    volatile uint64_t fg_done;    /* the last frame it handed to its queue */
    /* the game's side, again when fg_generation changes */
    uint32_t fg_generation;
    uint64_t fg_backbuffer, fg_hudless, fg_output[3], fg_fence; /* NT handles; hudless 0 when not given */
    uint32_t fg_w, fg_h, fg_format; /* the frame's size and DXGI format (the outputs' too) */
    uint32_t fg_count;            /* frames made between two: 1 (2x), up to 3 (4x, RTX 50) */
    /* each frame: the camera, without jitter (row vectors, D3D's), and the motion's scale to pixels (1: DLSS's; the helper
     * makes it a share of the frame for NVIDIA's) */
    volatile uint64_t fg_frame;   /* the fence value its inputs are ready at; the outputs at fg_frame + 1 */
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float cam_pos[3], cam_up[3], cam_right[3], cam_fwd[3];
    float cam_near, cam_far, cam_fov, cam_aspect;
    float fg_motion_scale[2];
    int32_t fg_reset;
    /* DLSS 5: the helper's side */
    volatile int32_t nr_available; /* 1 when it started on this GPU; 0 again if it fails later; nr_message says why */
    char nr_message[160];
    /* the game's side, each frame */
    int32_t nr;                   /* 1: the colour (R16G16B16A16_FLOAT) through DLSS 5 before DLSS */
    int32_t nr_style;             /* its look: 0 default, 1 natural, 2 cinematic */
    float nr_intensity;           /* its strength, 1 as it comes */
    /* frame generation, the helper's: the most frames this card makes between two (1 before RTX 50), and the
     * fg_generation it has made frame generation for (the game hands frames over once it's the game's: making it
     * takes seconds the first time) */
    int32_t fg_count_max;
    volatile uint32_t fg_made;
    /* DLSS, the helper's: the `generation` it has made DLSS for (the game hands frames over once it's the game's:
     * making it takes seconds with DLSS 5, or at a large size) */
    volatile uint32_t made;
    /* the game's, for the helper's timing (its log's "late" lines): QueryPerformanceCounter as it handed the last
     * frame over, DLSS's and frame generation's (one clock in both processes) */
    volatile int64_t posted_qpc;
    volatile int64_t fg_posted_qpc;
    /* DLSS 5's other controls, the game's each frame: its tone and structure strengths (1 as they come), the skin's
     * structure strength (-1: its own), and 1 to have it find its own mask */
    float nr_tone, nr_structure, nr_skin;
    int32_t nr_auto_mask;
    /* the helper's, for /vana stats: what the GPU spent in each of its jobs on the last frame it timed, from the
     * timestamps it writes round them on its own queue (ms; gpu_timed 1 once there are some): DLSS 5, DLSS, and one
     * frame made by frame generation. They're the GPU's own work, not the wait for it. */
    volatile int32_t gpu_timed;
    float gpu_nr_ms, gpu_sr_ms, gpu_fg_ms;
    /* the models asked for, the game's, with the textures (generation): DLSS's render preset (NVSDK_NGX_DLSS_Hint_Render_
     * Preset: 0 its own choice, 11 K, 13 M, 6 F...), and DLSS 5's (its own numbers; 0 the default) */
    int32_t sr_preset;
    int32_t nr_preset;
    /* the upscaler, the game's, set before it starts the helper: VANA_UPSCALER_DLSS or VANA_UPSCALER_FSR */
    int32_t upscaler;
    /* the game's: 1 has the helper end (it's let go: the game starts another for the other upscaler); set with ".go" */
    volatile int32_t quit;
    /* FSR's, the game's, with the textures (generation): the depth has no far plane (FFXI's is too far to count: 1 - near
     * / w), and the FSR version asked for (0 FSR's own choice: FSR 4 where it runs, else FSR 3.1; 2, 3 or 4 that major
     * version, where the DLL has it for this card) */
    int32_t depth_infinite;
    int32_t sr_version_want;
    /* FSR's, the game's each frame: the camera's near and far planes, its vertical field of view (radians), and the time
     * since the frame before (ms). `sharpness` (above) is FSR's own sharpening, 0 (off) to 1 */
    float sr_near, sr_far, sr_fov, sr_frame_ms;
    /* FSR's, the helper's: the version it runs ("FSR 4.1.1", "FSR 3.1.5"), once it's made for the textures (made) */
    char sr_version[48];
    /* frame generation's kind, the game's, with fg_generation: VANA_FG_DLSS or VANA_FG_FSR */
    int32_t fg_kind;
    /* AMD's, the helper's: 1 while it can be asked for (its DLLs beside the helper); 0 once making it failed, and
     * fg_fsr_message why. fg_version: the version made ("FSR FG 4.0.1"), with fg_made */
    volatile int32_t fg_fsr_available;
    char fg_fsr_message[160];
    char fg_version[48];
    /* AMD's, the game's each frame (with fg_frame): the frame's jitter (render pixels), the part of the depth and motion
     * drawn, the time since the frame before (ms) and the game's frame number (AMD's frame id: one more each frame, else
     * it starts afresh); the camera's planes and field of view are cam_near, cam_far and cam_fov above */
    float fg_jitter_x, fg_jitter_y, fg_frame_ms;
    uint32_t fg_render_w, fg_render_h;
    uint64_t fg_frame_id;
    /* DLSS 5 with the game's effects kept out of it ([graphics.effects] dlss5_fx_out). The game's, with the textures
     * (generation): fx, an NT handle to the effects' own picture (R16G16B16A16_FLOAT, the colour's size: what the game's
     * effects - spells, flames, glows - changed in the scene, the finished colour less the scene before them; 0 without
     * DLSS 5); and each frame nr_fx, 1 when the colour handed over is the scene before them and fx is to be added onto
     * DLSS 5's picture before DLSS. The helper's: nr_fx_ok, 1 once it has fx and can add it (a compute pass of its own;
     * 0: the game hands the colour over whole, effects and all, as without it) */
    uint64_t fx;
    int32_t nr_fx;
    volatile int32_t nr_fx_ok;
    /* DLSS 5's advanced settings (the /vana window's DLSS 5 > Advanced Settings). The game's, with the textures
     * (generation): nr_passes, how many times the colour goes through DLSS 5 (1 to 3), each pass a feature of its own (its
     * own history) on the pass before's picture, and nr_taper, the share of the local tone and structure strengths (and
     * the skin's, where set) passes 2 and 3 run at. Each frame: DLSS 5's picture blended back with the colour it was
     * handed (the helper's nrmix pass): nr_mix how far toward DLSS 5's picture (0 the colour as handed, 1 DLSS 5's,
     * past 1 further on), nr_colour how much of DLSS 5's own colour comes with it (0 the colour's hue and saturation
     * with DLSS 5's brightness, 1 DLSS 5's colour), nr_brighten the most it may brighten a pixel, times what it was (0: no
     * limit); all three at 1, 1, 0 skip the pass (DLSS 5's picture as it is) */
    int32_t nr_passes;
    float nr_taper;
    float nr_mix, nr_colour, nr_brighten;
    /* The helper's, as it goes: what it's doing ("running DLSS 5, pass 1 of 2") and on which of the game's frames (the fence
     * value it was handed; 0 none). The game reads them once the helper has ended, for why it crashed: there even when
     * Windows ended it on the spot, without its crash report (a fail-fast, heap corruption) */
    volatile uint64_t doing_frame;
    char doing[96];
    /* The game's, from the debug window's DLSS crash tests (/vana debug): a VANA_CRASH_TEST_*, and the helper fails that way at
     * its next frame's upscale, where a real crash would come, to try what one leaves (the chat, vanacore.log, the dump) */
    volatile int32_t crash_test;
    /* The game's, with the textures (generation): the size DLSS 5 runs at, a share of the colour's each way (1, or 0: the
     * colour's). Below 1 the helper hands it the colour, depth and motion made smaller, and lays the change it made back
     * onto the colour at its own size (nrscale) */
    float nr_scale;
    /* The helper's, once it has made DLSS 5: the size it runs at (nr_scale's, or the colour's where the smaller one couldn't
     * be made); 0 without it. The /vana DLSS panel's Resolution line */
    volatile uint32_t nr_w, nr_h;
    /* The game's, with the textures (generation): NT handles to its masks of what isn't the static world (R8_UNORM, the
     * colour's size; gfx_d3d12.c fx_dlssr), 0 without. reactive: the game's effects (every blended draw after the scene:
     * spells, glows, a maw's vortex; found from the scene with and without them) and every depth-writing thing that moves
     * on its own (characters, plants, VanaCore's grass, a boat), how far the upscaler is to lean on this frame's pixels
     * there instead of its history: FSR's reactive mask, and DLSS's bias-current-colour mask. tc: the effects alone,
     * FSR's transparency and composition mask (where the depth and motion don't describe what's seen). The motion handed
     * over is the camera's alone unless [graphics.effects] dlss_motion_all asks for the moving things' own, so without these
     * a character or a spell is fetched from where the wall behind it was, and smears */
    uint64_t reactive;
    uint64_t tc;
    /* The game's, set before it starts the helper: 1 when no fence is shared between the two processes (`fence` and
     * `fg_fence` stay 0), because Wine and Proton can't open one from another process (vkd3d-proton imports it with
     * vkImportSemaphoreWin32HandleKHR, which dies in an access violation: "d3d12 fence from other process"). The fences are
     * each side's own, and a CPU wait does what the GPU's wait did: the game's thread waits for its queue to reach `frame`
     * and then sets ".go" (".fg"); the helper skips its wait on the game's fence, runs, waits for its own queue to finish,
     * sets `done` (`fg_done`) and then the events ".done" (".fgdone"); and the game signals its own fence to frame + 1 from
     * a thread of its own. The helper opens ".done" and ".fgdone" only with this set */
    int32_t cpu_sync;
} VanaDlssShared;

enum {
    VANA_CRASH_TEST_NONE = 0,
    VANA_CRASH_TEST_AV = 1,       /* an access violation on the thread serving the game: the full report */
    VANA_CRASH_TEST_THREAD = 2,   /* ... on a thread of its own, as a driver's thread would */
    VANA_CRASH_TEST_CPP = 3,      /* an unhandled C++ exception */
    VANA_CRASH_TEST_FASTFAIL = 4, /* a fail-fast: Windows ends it on the spot, with no report (the game has the code and doing) */
    VANA_CRASH_TEST_HANG = 5      /* it stops answering, with the game's frame in hand */
};

#endif
