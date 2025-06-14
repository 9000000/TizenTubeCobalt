// Copyright 2014 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cobalt/browser/browser_module.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/bind.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/optional.h"
#include "base/path_service.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_tokenizer.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "cobalt/base/cobalt_paths.h"
#include "cobalt/base/init_cobalt.h"
#include "cobalt/base/source_location.h"
#include "cobalt/base/task_runner_util.h"
#include "cobalt/base/tokens.h"
#include "cobalt/browser/cpu_usage_tracker.h"
#include "cobalt/browser/on_screen_keyboard_extension_bridge.h"
#include "cobalt/browser/screen_shot_writer.h"
#include "cobalt/browser/switches.h"
#include "cobalt/browser/user_agent_platform_info.h"
#include "cobalt/configuration/configuration.h"
#include "cobalt/cssom/viewport_size.h"
#include "cobalt/dom/input_event_init.h"
#include "cobalt/dom/keyboard_event_init.h"
#include "cobalt/dom/keycode.h"
#include "cobalt/dom/mutation_observer_task_manager.h"
#include "cobalt/dom/navigator.h"
#include "cobalt/dom/window.h"
#include "cobalt/h5vcc/h5vcc.h"
#include "cobalt/input/input_device_manager_fuzzer.h"
#include "cobalt/math/matrix3_f.h"
#include "cobalt/overlay_info/overlay_info_registry.h"
#include "cobalt/persistent_storage/persistent_settings.h"
#include "cobalt/trace_event/scoped_trace_to_file.h"
#include "cobalt/ui_navigation/scroll_engine/scroll_engine.h"
#include "cobalt/web/csp_delegate_factory.h"
#include "cobalt/web/navigator_ua_data.h"
#include "starboard/atomic.h"
#include "starboard/common/time.h"
#include "starboard/configuration.h"
#include "starboard/extension/graphics.h"
#include "starboard/system.h"
#include "third_party/icu/source/i18n/unicode/timezone.h"

using cobalt::cssom::ViewportSize;

namespace cobalt {

namespace browser {
namespace {

// This constant defines the maximum rate at which the layout engine will
// refresh over time.  Since there is little benefit in performing a layout
// faster than the display's refresh rate, we set this to 60Hz.
const float kLayoutMaxRefreshFrequencyInHz = 60.0f;

// TODO: Subscribe to viewport size changes.

const int kMainWebModuleZIndex = 1;
const int kSplashScreenZIndex = 2;
#if defined(ENABLE_DEBUGGER)
const int kDebugConsoleZIndex = 3;
#endif  // defined(ENABLE_DEBUGGER)
const int kOverlayInfoZIndex = 4;

#if defined(ENABLE_DEBUGGER)

const char kFuzzerToggleCommand[] = "fuzzer_toggle";
const char kFuzzerToggleCommandShortHelp[] = "Toggles the input fuzzer on/off.";
const char kFuzzerToggleCommandLongHelp[] =
    "Each time this is called, it will toggle whether the input fuzzer is "
    "activated or not.  While activated, input will constantly and randomly be "
    "generated and passed directly into the main web module.";

#if defined(ENABLE_DEBUGGER)
// Command to reload the current URL.
const char kBoxDumpCommand[] = "boxdump";

// Help strings for the navigate command.
const char kBoxDumpCommandShortHelp[] = "Return a box dump.";
const char kBoxDumpCommandLongHelp[] =
    "Returns a dump of the most recent layout box tree.";
#endif

const char kScreenshotCommand[] = "screenshot";
const char kScreenshotCommandShortHelp[] = "Takes a screenshot.";
const char kScreenshotCommandLongHelp[] =
    "Creates a screenshot of the most recent layout tree and writes it "
    "to disk. Logs the filename of the screenshot to the console when done.";

// Debug console command `disable_media_codecs` for disabling a list of codecs
// by treating them as unsupported
const char kDisableMediaCodecsCommand[] = "disable_media_codecs";
const char kDisableMediaCodecsCommandShortHelp[] =
    "Specify a semicolon-separated list of disabled media codecs.";
const char kDisableMediaCodecsCommandLongHelp[] =
    "Disabling Media Codecs will force the app to claim they are not "
    "supported. This "
    "is useful when trying to target testing to certain codecs, since other "
    "codecs will get picked as a fallback as a result.";

const char kNavigateTimedTrace[] = "navigate_timed_trace";
const char kNavigateTimedTraceShortHelp[] =
    "Request a timed trace from the next navigation.";
const char kNavigateTimedTraceLongHelp[] =
    "When this is called, a timed trace will start at the next navigation "
    "and run for the given number of seconds.";

void ScreenshotCompleteCallback(const base::FilePath& output_path) {
  DLOG(INFO) << "Screenshot written to " << output_path.value();
}

void OnScreenshotMessage(BrowserModule* browser_module,
                         const std::string& message) {
  base::FilePath dir;
  if (!base::PathService::Get(cobalt::paths::DIR_COBALT_DEBUG_OUT, &dir)) {
    NOTREACHED() << "Failed to get debug out directory.";
  }

  base::Time::Exploded exploded;
  base::Time::Now().LocalExplode(&exploded);
  DCHECK(exploded.HasValidValues());
  std::string screenshot_file_name =
      base::StringPrintf("screenshot-%04d-%02d-%02d_%02d-%02d-%02d.png",
                         exploded.year, exploded.month, exploded.day_of_month,
                         exploded.hour, exploded.minute, exploded.second);

  base::FilePath output_path = dir.Append(screenshot_file_name);
  browser_module->RequestScreenshotToFile(
      output_path, loader::image::EncodedStaticImage::ImageFormat::kPNG,
      /*clip_rect=*/base::nullopt,
      base::Bind(&ScreenshotCompleteCallback, output_path));
}

#endif  // defined(ENABLE_DEBUGGER)

renderer::RendererModule::Options RendererModuleWithCameraOptions(
    renderer::RendererModule::Options options,
    scoped_refptr<input::Camera3D> camera_3d) {
  options.get_camera_transform = base::Bind(
      &input::Camera3D::GetCameraTransformAndUpdateOrientation, camera_3d);
  return options;  // Copy.
}

renderer::Submission CreateSubmissionFromLayoutResults(
    const browser::WebModule::LayoutResults& layout_results) {
  renderer::Submission renderer_submission(layout_results.render_tree,
                                           layout_results.layout_time);
  if (!layout_results.on_rasterized_callback.is_null()) {
    renderer_submission.on_rasterized_callbacks.push_back(
        layout_results.on_rasterized_callback);
  }
  return renderer_submission;
}

}  // namespace

BrowserModule::BrowserModule(const GURL& url,
                             base::ApplicationState initial_application_state,
                             base::EventDispatcher* event_dispatcher,
                             network::NetworkModule* network_module,
#if SB_IS(EVERGREEN)
                             updater::UpdaterModule* updater_module,
#endif
                             const Options& options,
                             bool enable_skia_rasterizer)
    : ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(this)),
      ALLOW_THIS_IN_INITIALIZER_LIST(
          weak_this_(weak_ptr_factory_.GetWeakPtr())),
      options_(options),
      task_runner_(base::SequencedTaskRunner::GetCurrentDefault()),
      event_dispatcher_(event_dispatcher),
      is_rendered_(false),
      is_web_module_rendered_(false),
      can_play_type_handler_(media::MediaModule::CreateCanPlayTypeHandler()),
      network_module_(network_module),
#if SB_IS(EVERGREEN)
      updater_module_(updater_module),
#endif
      splash_screen_cache_(new SplashScreenCache()),
      web_module_loaded_(base::WaitableEvent::ResetPolicy::MANUAL,
                         base::WaitableEvent::InitialState::NOT_SIGNALED),
      web_module_created_callback_(options_.web_module_created_callback),
      navigate_time_("Time.Browser.Navigate", 0,
                     "The last time a navigation occurred."),
      on_load_event_time_("Time.Browser.OnLoadEvent", 0,
                          "The last time the window.OnLoad event fired."),
      javascript_reserved_memory_(
          "Memory.JS", 0,
          "The total memory that is reserved by the JavaScript engine, which "
          "includes both parts that have live JavaScript values, as well as "
          "preallocated space for future values."),
      disabled_media_codecs_(
          "Media.DisabledMediaCodecs", "",
          "List of codecs that should currently be reported as unsupported."),
#if defined(ENABLE_DEBUGGER)
      ALLOW_THIS_IN_INITIALIZER_LIST(fuzzer_toggle_command_handler_(
          kFuzzerToggleCommand,
          base::Bind(&BrowserModule::OnFuzzerToggle, base::Unretained(this)),
          kFuzzerToggleCommandShortHelp, kFuzzerToggleCommandLongHelp)),
#if defined(ENABLE_DEBUGGER)
      ALLOW_THIS_IN_INITIALIZER_LIST(boxdump_command_handler_(
          kBoxDumpCommand,
          base::Bind(&BrowserModule::OnBoxDumpMessage, base::Unretained(this)),
          kBoxDumpCommandShortHelp, kBoxDumpCommandLongHelp)),
#endif  // defined(ENABLE_DEBUGGER)
      ALLOW_THIS_IN_INITIALIZER_LIST(screenshot_command_handler_(
          kScreenshotCommand,
          base::Bind(&OnScreenshotMessage, base::Unretained(this)),
          kScreenshotCommandShortHelp, kScreenshotCommandLongHelp)),
      ALLOW_THIS_IN_INITIALIZER_LIST(disable_media_codecs_command_handler_(
          kDisableMediaCodecsCommand,
          base::Bind(&BrowserModule::OnDisableMediaCodecs,
                     base::Unretained(this)),
          kDisableMediaCodecsCommandShortHelp,
          kDisableMediaCodecsCommandLongHelp)),
      ALLOW_THIS_IN_INITIALIZER_LIST(navigate_timed_trace_command_handler_(
          kNavigateTimedTrace,
          base::Bind(&BrowserModule::OnNavigateTimedTrace,
                     base::Unretained(this)),
          kNavigateTimedTraceShortHelp, kNavigateTimedTraceLongHelp)),
#endif  // defined(ENABLE_DEBUGGER)
      has_resumed_(base::WaitableEvent::ResetPolicy::MANUAL,
                   base::WaitableEvent::InitialState::NOT_SIGNALED),
      on_error_retry_count_(0),
      waiting_for_error_retry_(false),
      application_state_(initial_application_state),
      main_web_module_generation_(0),
      next_timeline_id_(1),
      current_splash_screen_timeline_id_(-1),
      current_main_web_module_timeline_id_(-1),
      enable_skia_rasterizer_(enable_skia_rasterizer) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::BrowserModule()");

  if (options.enable_on_screen_keyboard) {
    if (OnScreenKeyboardExtensionBridge::IsSupported()) {
      const CobaltExtensionOnScreenKeyboardApi* on_screen_keyboard_extension =
          static_cast<const CobaltExtensionOnScreenKeyboardApi*>(
              SbSystemGetExtension(kCobaltExtensionOnScreenKeyboardName));
      on_screen_keyboard_bridge_ =
          std::make_unique<OnScreenKeyboardExtensionBridge>(
              base::Bind(&BrowserModule::GetSbWindow, base::Unretained(this)),
              on_screen_keyboard_extension);
    }
  } else {
    on_screen_keyboard_bridge_ = NULL;
  }

  // Apply platform memory setting adjustments and defaults.
  ApplyAutoMemSettings();

  platform_info_.reset(
      new browser::UserAgentPlatformInfo(enable_skia_rasterizer_));
  service_worker_registry_.reset(new ServiceWorkerRegistry(
      &web_settings_, network_module, platform_info_.get()));

  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  // Create the main web module layer.
  main_web_module_layer_ =
      render_tree_combiner_.CreateLayer(kMainWebModuleZIndex);
  // Create the splash screen layer.
  splash_screen_layer_ = render_tree_combiner_.CreateLayer(kSplashScreenZIndex);
// Create the debug console layer.
#if defined(ENABLE_DEBUGGER)
  if (DebugConsole::IsEnabled()) {
    debug_console_layer_ =
        render_tree_combiner_.CreateLayer(kDebugConsoleZIndex);
  }
#endif

  int qr_code_overlay_slots = 4;
  if (command_line->HasSwitch(switches::kQrCodeOverlay)) {
    auto slots_in_string =
        command_line->GetSwitchValueASCII(switches::kQrCodeOverlay);
    if (!slots_in_string.empty()) {
      auto result = base::StringToInt(slots_in_string, &qr_code_overlay_slots);
      DCHECK(result) << "Failed to convert value of --"
                     << switches::kQrCodeOverlay << ": "
                     << qr_code_overlay_slots << " to int.";
      DCHECK_GT(qr_code_overlay_slots, 0);
    }
    qr_overlay_info_layer_ =
        render_tree_combiner_.CreateLayer(kOverlayInfoZIndex);
  } else {
    overlay_info::OverlayInfoRegistry::Disable();
  }

  // Setup our main web module to have the H5VCC API injected into it.
  DCHECK(!ContainsKey(
      options_.web_module_options.injected_global_object_attributes, "h5vcc"));
  DCHECK(!ContainsKey(
      options_.web_module_options.web_options.injected_global_object_attributes,
      "h5vcc"));
  options_.web_module_options.injected_global_object_attributes["h5vcc"] =
      base::Bind(&BrowserModule::CreateH5vccCallback, base::Unretained(this));

  CpuUsageTracker::GetInstance()->Initialize(options_.persistent_settings);

  if (command_line->HasSwitch(switches::kDisableTimerResolutionLimit)) {
    options_.web_module_options.limit_performance_timer_resolution = false;
  }

  options_.web_module_options.enable_inline_script_warnings =
      !command_line->HasSwitch(switches::kSilenceInlineScriptWarnings);

#if defined(ENABLE_DEBUGGER) && defined(ENABLE_DEBUG_COMMAND_LINE_SWITCHES)
  if (command_line->HasSwitch(switches::kInputFuzzer)) {
    OnFuzzerToggle(std::string());
  }
  if (command_line->HasSwitch(switches::kSuspendFuzzer)) {
    suspend_fuzzer_.emplace();
  }
#endif  // ENABLE_DEBUGGER && ENABLE_DEBUG_COMMAND_LINE_SWITCHES

#if defined(ENABLE_DEBUG_COMMAND_LINE_SWITCHES)
  if (command_line->HasSwitch(switches::kDisableMediaCodecs)) {
    std::string codecs =
        command_line->GetSwitchValueASCII(switches::kDisableMediaCodecs);
    if (!codecs.empty()) {
#if defined(ENABLE_DEBUGGER)
      OnDisableMediaCodecs(codecs);
#else   // ENABLE_DEBUGGER
      // Here command line switches are enabled but the debug console is not.
      can_play_type_handler_->SetDisabledMediaCodecs(codecs);
#endif  // ENABLE_DEBUGGER
    }
  }

  if (command_line->HasSwitch(switches::kDisableMediaEncryptionSchemes)) {
    std::string encryption_schemes = command_line->GetSwitchValueASCII(
        switches::kDisableMediaEncryptionSchemes);
    if (!encryption_schemes.empty()) {
      can_play_type_handler_->SetDisabledMediaEncryptionSchemes(
          encryption_schemes);
    }
  }
#endif  // ENABLE_DEBUG_COMMAND_LINE_SWITCHES

  if (application_state_ == base::kApplicationStateStarted ||
      application_state_ == base::kApplicationStateBlurred) {
    InitializeSystemWindow();
  }

  resource_provider_stub_.emplace(true /*allocate_image_data*/);

#if defined(ENABLE_DEBUGGER)
  if (debug_console_layer_) {
    debug_console_.reset(new DebugConsole(
        platform_info_.get(), application_state_,
        base::Bind(&BrowserModule::QueueOnDebugConsoleRenderTreeProduced,
                   base::Unretained(this)),
        &web_settings_, network_module_, GetViewportSize(),
        GetResourceProvider(), kLayoutMaxRefreshFrequencyInHz,
        base::Bind(&BrowserModule::CreateDebugClient, base::Unretained(this)),
        base::Bind(&BrowserModule::OnMaybeFreeze, base::Unretained(this))));
    lifecycle_observers_.AddObserver(debug_console_.get());
  }
#endif  // defined(ENABLE_DEBUGGER)

  const renderer::Pipeline* pipeline =
      renderer_module_ ? renderer_module_->pipeline() : nullptr;
  if (command_line->HasSwitch(switches::kDisableMapToMesh) ||
      !renderer::Pipeline::IsMapToMeshEnabled(pipeline)) {
    options_.web_module_options.enable_map_to_mesh = false;
  }

  if (qr_overlay_info_layer_) {
    math::Size width_height = GetViewportSize().width_height();
    qr_code_overlay_.reset(new overlay_info::QrCodeOverlay(
        width_height, qr_code_overlay_slots, GetResourceProvider(),
        base::Bind(&BrowserModule::QueueOnQrCodeOverlayRenderTreeProduced,
                   base::Unretained(this))));
  }

  if (command_line->HasSwitch(switches::kDisableProgressivePlayback)) {
    can_play_type_handler_->DisableProgressiveSupport();
  }

  // Set the fallback splash screen url to the default fallback url.
  fallback_splash_screen_url_ = options.fallback_splash_screen_url;

  // Synchronously construct our WebModule object.
  Navigate(url);
  DCHECK(web_module_);
}

BrowserModule::~BrowserModule() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  // Transition into the suspended state from whichever state we happen to
  // currently be in, to prepare for shutdown.
  int64_t now = starboard::CurrentMonotonicTime();
  switch (application_state_) {
    case base::kApplicationStateStarted:
      Blur(now);
      FALLTHROUGH;
    case base::kApplicationStateBlurred:
      Conceal(now);
      FALLTHROUGH;
    case base::kApplicationStateConcealed:
      Freeze(now);
      break;
    case base::kApplicationStateStopped:
      NOTREACHED() << "BrowserModule does not support the stopped state.";
      break;
    case base::kApplicationStateFrozen:
      break;
  }

  on_error_retry_timer_.Stop();

#if defined(ENABLE_DEBUGGER)
  if (debug_console_) {
    lifecycle_observers_.RemoveObserver(debug_console_.get());
  }
  debug_console_.reset();
#endif

  DestroySplashScreen();
  DestroyScrollEngine();
  // Make sure the WebModule is destroyed before the ServiceWorkerRegistry
  if (web_module_) {
    lifecycle_observers_.RemoveObserver(web_module_.get());
  }
  web_module_.reset();
}

void BrowserModule::Navigate(const GURL& url_reference) {
  if (network_module_) {
    // If protocolfilter setting was updated, the setting does not take effect
    // until the next page load.
    network_module_->SetProtocolFilterFromPersistentSettings();
  }

  // The argument is sometimes |pending_navigate_url_|, and Navigate can modify
  // |pending_navigate_url_|, so we want to keep a copy of the argument to
  // preserve its original value.
  GURL url = url_reference;
  DLOG(INFO) << "In BrowserModule::Navigate " << url;
  TRACE_EVENT1("cobalt::browser", "BrowserModule::Navigate()", "url",
               url.spec());

  // Reset the waitable event regardless of the thread. This ensures that the
  // webdriver won't incorrectly believe that the webmodule has finished loading
  // when it calls Navigate() and waits for the |web_module_loaded_| signal.
  web_module_loaded_.Reset();

  // Repost to our own task runner if necessary.
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::Navigate, weak_this_, url));
    return;
  }

  // First try any registered handlers. If one of these handles the URL, we
  // don't use the web module.
  if (NavigateTryURLHandlers(url)) {
    return;
  }

  // Clear error handling once we're told to navigate, either because it's the
  // retry from the error or something decided we should navigate elsewhere.
  NavigateResetErrorHandling();

  // Navigations aren't allowed if the app is frozen. If this is the case,
  // simply set the pending navigate url, which will cause the navigation to
  // occur when Cobalt resumes, and return.
  if (NavigateHandleStateFrozen(url)) {
    return;
  }

  // Destroys the old WebModule, increments the navigation generation
  // number, and resets the main WebModule layer
  NavigateResetWebModule();

  // checks whether a service worker should be started for the given URL
  auto service_worker_started_event = std::make_unique<base::WaitableEvent>();
  bool can_start_service_worker =
      NavigateServiceWorkerSetups(url, service_worker_started_event.get());

  // Wait until after the old WebModule is destroyed before setting the navigate
  // time so that it won't be included in the time taken to load the URL.
  navigate_time_ = base::TimeTicks::Now().ToInternalValue();

  const ViewportSize viewport_size = GetViewportSize();

  // Show a splash screen while we're waiting for the web page to load.
  NavigateSetupSplashScreen(url, viewport_size);

  NavigateSetupScrollEngine();

  NavigateCreateWebModule(url, can_start_service_worker,
                          service_worker_started_event.get(), viewport_size);
}

void BrowserModule::NavigateResetWebModule() {
#if defined(ENABLE_DEBUGGER)
  if (web_module_) {
    web_module_->FreezeDebugger(&debugger_state_);
  }
#endif  // defined(ENABLE_DEBUGGER)
  // Destroy old WebModule first, so we don't get a memory high-watermark after
  // the second WebModule's constructor runs, but before
  // std::unique_ptr::reset() is run.
  if (web_module_) {
    lifecycle_observers_.RemoveObserver(web_module_.get());
  }
  web_module_.reset();

  // Increment the navigation generation so that we can attach it to event
  // callbacks as a way of identifying the new web module from the old ones.
  ++main_web_module_generation_;
  current_splash_screen_timeline_id_ = next_timeline_id_++;
  current_main_web_module_timeline_id_ = next_timeline_id_++;

  main_web_module_layer_->Reset();

#if defined(ENABLE_DEBUGGER)
  // Check to see if a timed_trace has been set, indicating that we should
  // begin a timed trace upon startup.
  if (navigate_timed_trace_duration_ != base::TimeDelta()) {
    trace_event::TraceToFileForDuration(
        base::FilePath(FILE_PATH_LITERAL("timed_trace.json")),
        navigate_timed_trace_duration_);
    navigate_timed_trace_duration_ = base::TimeDelta();
  }
#endif  // defined(ENABLE_DEBUGGER)
}

void BrowserModule::NavigateResetErrorHandling() {
  on_error_retry_timer_.Stop();
  waiting_for_error_retry_ = false;
}

bool BrowserModule::NavigateHandleStateFrozen(const GURL& url) {
  if (application_state_ == base::kApplicationStateFrozen) {
    pending_navigate_url_ = url;
    return true;
  }

  // Now that we know the navigation is occurring, clear out
  // |pending_navigate_url_|.
  pending_navigate_url_ = GURL::EmptyGURL();
  return false;
}

bool BrowserModule::NavigateServiceWorkerSetups(
    const GURL& url, base::WaitableEvent* service_worker_started_event) {
  // Service worker should only start for HTTP or HTTPS fetches.
  // https://fetch.spec.whatwg.org/commit-snapshots/8f8ab504da6ca9681db5c7f8aa3d1f4b6bf8840c/#http-fetch
  bool can_start_service_worker = url.SchemeIsHTTPOrHTTPS();
  watchdog::Watchdog* watchdog = watchdog::Watchdog::GetInstance();
  if (watchdog) {
    std::vector<std::string> service_worker_clients = {
        worker::WorkerConsts::kServiceWorkerRegistryName,
        worker::WorkerConsts::kServiceWorkerName};
    std::string violation_json =
        watchdog->GetWatchdogViolations(service_worker_clients);
    {
      if (violation_json != "") {
        LOG(WARNING) << "Service Worker watchdog violation detected: "
                     << violation_json;
        LOG(WARNING) << "Erase Service Worker registration map.";
        can_start_service_worker = false;
        service_worker_registry_->EraseRegistrationMap();
      }
    }
  }
  if (can_start_service_worker) {
    service_worker_registry_->EnsureServiceWorkerStarted(
        url::Origin::Create(url), url, service_worker_started_event);
  }
  return can_start_service_worker;
}

void BrowserModule::NavigateSetupSplashScreen(
    const GURL& url, const ViewportSize viewport_size) {
  DestroySplashScreen();
  if (options_.enable_splash_screen_on_reloads ||
      main_web_module_generation_ == 1) {
    base::Optional<std::string> topic = SetSplashScreenTopicFallback(url);
    splash_screen_cache_->SetUrl(url, topic);

    if (fallback_splash_screen_url_ ||
        splash_screen_cache_->IsSplashScreenCached()) {
      splash_screen_.reset(new SplashScreen(
          platform_info_.get(), application_state_,
          base::Bind(&BrowserModule::QueueOnSplashScreenRenderTreeProduced,
                     base::Unretained(this)),
          &web_settings_, network_module_, viewport_size, GetResourceProvider(),
          kLayoutMaxRefreshFrequencyInHz, fallback_splash_screen_url_,
          splash_screen_cache_.get(),
          base::Bind(&BrowserModule::DestroySplashScreen, weak_this_),
          base::Bind(&BrowserModule::OnMaybeFreeze, base::Unretained(this))));
      lifecycle_observers_.AddObserver(splash_screen_.get());
    }
  }
}

void BrowserModule::NavigateSetupScrollEngine() {
  DestroyScrollEngine();
  scroll_engine_.reset(new ui_navigation::scroll_engine::ScrollEngine());
  lifecycle_observers_.AddObserver(scroll_engine_.get());
  scroll_engine_->thread()->Start();
}

void BrowserModule::NavigateCreateWebModule(
    const GURL& url, bool can_start_service_worker,
    base::WaitableEvent* service_worker_started_event,
    const ViewportSize viewport_size) {
// Create new WebModule.
#if !defined(COBALT_FORCE_CSP)
  options_.web_module_options.csp_insecure_allowed_token =
      web::CspDelegateFactory::GetInsecureAllowedToken();
#endif
  WebModule::Options options(options_.web_module_options);
  options.splash_screen_cache = splash_screen_cache_.get();
  options.navigation_callback =
      base::Bind(&BrowserModule::Navigate, base::Unretained(this));
  options.loaded_callbacks.push_back(
      base::Bind(&BrowserModule::OnLoad, base::Unretained(this)));
#if defined(ENABLE_FAKE_MICROPHONE)
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kFakeMicrophone)) {
    options.dom_settings_options.microphone_options.enable_fake_microphone =
        true;
  }
#endif  // defined(ENABLE_FAKE_MICROPHONE)
  if (on_screen_keyboard_bridge_) {
    options.on_screen_keyboard_bridge = on_screen_keyboard_bridge_.get();
  }
  options.image_cache_capacity_multiplier_when_playing_video =
      configuration::Configuration::GetInstance()
          ->CobaltImageCacheCapacityMultiplierWhenPlayingVideo();
  if (input_device_manager_) {
    options.camera_3d = input_device_manager_->camera_3d();
  }

  options.provide_screenshot_function =
      base::Bind(&BrowserModule::RequestScreenshotToMemoryUnencoded,
                 base::Unretained(this));

#if defined(ENABLE_DEBUGGER)
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kWaitForWebDebugger)) {
    int wait_for_generation =
        atoi(base::CommandLine::ForCurrentProcess()
                 ->GetSwitchValueASCII(switches::kWaitForWebDebugger)
                 .c_str());
    if (wait_for_generation < 1) wait_for_generation = 1;
    options.wait_for_web_debugger =
        (wait_for_generation == main_web_module_generation_);
  }

  options.debugger_state = debugger_state_.get();
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableWebDebugger)) {
    options.enable_debugger = true;
  }
#endif  // ENABLE_DEBUGGER

  // Pass down this callback from to Web module.
  options.maybe_freeze_callback =
      base::Bind(&BrowserModule::OnMaybeFreeze, base::Unretained(this));

  options.web_options.web_settings = &web_settings_;
  options.web_options.network_module = network_module_;
  options.web_options.service_worker_context =
      service_worker_registry_->service_worker_context();
  options.web_options.platform_info = platform_info_.get();
  web_module_.reset(new WebModule("MainWebModule"));
  // Wait for service worker to start if one exists.
  if (can_start_service_worker) {
    service_worker_started_event->Wait();
  }
  web_module_->Run(
      url, application_state_, scroll_engine_.get(),
      base::Bind(&BrowserModule::QueueOnRenderTreeProduced,
                 base::Unretained(this), main_web_module_generation_),
      base::Bind(&BrowserModule::OnError, base::Unretained(this)),
      base::Bind(&BrowserModule::OnWindowClose, base::Unretained(this)),
      base::Bind(&BrowserModule::OnWindowMinimize, base::Unretained(this)),
      can_play_type_handler_.get(), media_module_.get(), viewport_size,
      GetResourceProvider(), kLayoutMaxRefreshFrequencyInHz, options);
  lifecycle_observers_.AddObserver(web_module_.get());

  if (system_window_) {
    web_module_->GetUiNavRoot()->SetContainerWindow(
        system_window_->GetSbWindow());
  }
}

void BrowserModule::Reload() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Reload()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(web_module_);
  web_module_->ExecuteJavascript(
      "location.reload();",
      base::SourceLocation("[object BrowserModule]", 1, 1));
}

void BrowserModule::OnLoad() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnLoad()");
  // Repost to our own task runner if necessary. This also prevents
  // asynchronous access to this object by |web_module_| during destruction.
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&BrowserModule::OnLoad, weak_this_));
    return;
  }

  // This log is relied on by the webdriver benchmark tests, so it shouldn't be
  // changed unless the corresponding benchmark logic is changed as well.
  LOG(INFO) << "Loaded WebModule";

  // Clear |on_error_retry_count_| after a successful load.
  on_error_retry_count_ = 0;

  on_load_event_time_ = base::TimeTicks::Now().ToInternalValue();

#if SB_IS(EVERGREEN)
  if (updater_module_) {
    // Mark the Evergreen update successful if this is Evergreen-Full.
    updater_module_->MarkSuccessful();
  }
#endif

  web_module_loaded_.Signal();

  options_.persistent_settings->Validate();
  ValidateCacheBackendSettings();
}

bool BrowserModule::WaitForLoad(const base::TimeDelta& timeout) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::WaitForLoad()");
  return web_module_loaded_.TimedWait(timeout);
}

void BrowserModule::EnsureScreenShotWriter() {
  if (!screen_shot_writer_ && renderer_module_) {
    screen_shot_writer_.reset(
        new ScreenShotWriter(renderer_module_->pipeline()));
  }
}

#if defined(ENABLE_WEBDRIVER) || defined(ENABLE_DEBUGGER)
void BrowserModule::RequestScreenshotToFile(
    const base::FilePath& path,
    loader::image::EncodedStaticImage::ImageFormat image_format,
    const base::Optional<math::Rect>& clip_rect,
    const base::Closure& done_callback) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::RequestScreenshotToFile()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::RequestScreenshotToFile,
                              base::Unretained(this), path, image_format,
                              clip_rect, std::move(done_callback)));
    return;
  }
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  EnsureScreenShotWriter();
  DCHECK(screen_shot_writer_);
  if (!screen_shot_writer_) return;

  scoped_refptr<render_tree::Node> render_tree;
  web_module_->DoSynchronousLayoutAndGetRenderTree(&render_tree);
  if (!render_tree) {
    LOG(WARNING) << "Unable to get animated render tree";
    return;
  }

  screen_shot_writer_->RequestScreenshotToFile(image_format, path, render_tree,
                                               clip_rect, done_callback);
}

void BrowserModule::RequestScreenshotToMemory(
    loader::image::EncodedStaticImage::ImageFormat image_format,
    const base::Optional<math::Rect>& clip_rect,
    const ScreenShotWriter::ImageEncodeCompleteCallback& screenshot_ready) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::RequestScreenshotToMemory()");
  EnsureScreenShotWriter();
  DCHECK(screen_shot_writer_);
  if (!screen_shot_writer_) return;
  // Note: This does not have to be called from task_runner_.

  scoped_refptr<render_tree::Node> render_tree;
  web_module_->DoSynchronousLayoutAndGetRenderTree(&render_tree);
  if (!render_tree) {
    LOG(WARNING) << "Unable to get animated render tree";
    return;
  }

  screen_shot_writer_->RequestScreenshotToMemory(image_format, render_tree,
                                                 clip_rect, screenshot_ready);
}
#endif  // defined(ENABLE_WEBDRIVER) || defined(ENABLE_DEBUGGER)

// Request a screenshot to memory without compressing the image.
void BrowserModule::RequestScreenshotToMemoryUnencoded(
    const scoped_refptr<render_tree::Node>& render_tree_root,
    const base::Optional<math::Rect>& clip_rect,
    const renderer::Pipeline::RasterizationCompleteCallback& callback) {
  EnsureScreenShotWriter();
  DCHECK(screen_shot_writer_);
  if (!screen_shot_writer_) return;
  screen_shot_writer_->RequestScreenshotToMemoryUnencoded(render_tree_root,
                                                          clip_rect, callback);
}

void BrowserModule::ProcessRenderTreeSubmissionQueue() {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::ProcessRenderTreeSubmissionQueue()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // If the app is preloaded, clear the render tree queue to avoid unnecessary
  // rendering overhead.
  if (application_state_ == base::kApplicationStateConcealed) {
    render_tree_submission_queue_.ClearAll();
  } else {
    render_tree_submission_queue_.ProcessAll();
  }
}

void BrowserModule::QueueOnRenderTreeProduced(
    int main_web_module_generation,
    const browser::WebModule::LayoutResults& layout_results) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::QueueOnRenderTreeProduced()");
  render_tree_submission_queue_.AddMessage(
      base::Bind(&BrowserModule::OnRenderTreeProduced, base::Unretained(this),
                 main_web_module_generation, layout_results));
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&BrowserModule::ProcessRenderTreeSubmissionQueue, weak_this_));
}

void BrowserModule::QueueOnSplashScreenRenderTreeProduced(
    const browser::WebModule::LayoutResults& layout_results) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::QueueOnSplashScreenRenderTreeProduced()");
  render_tree_submission_queue_.AddMessage(
      base::Bind(&BrowserModule::OnSplashScreenRenderTreeProduced,
                 base::Unretained(this), layout_results));
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&BrowserModule::ProcessRenderTreeSubmissionQueue, weak_this_));
}

void BrowserModule::OnRenderTreeProduced(
    int main_web_module_generation,
    const browser::WebModule::LayoutResults& layout_results) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnRenderTreeProduced()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  if (main_web_module_generation != main_web_module_generation_) {
    // Ignore render trees produced by old stale web modules.  This might happen
    // during a navigation transition.
    return;
  }

  if (splash_screen_) {
    if (on_screen_keyboard_show_called_) {
      // Hide the splash screen as quickly as possible.
      DestroySplashScreen();
    } else if (!splash_screen_->ShutdownSignaled()) {
      splash_screen_->Shutdown();
    }
  }
  if (application_state_ == base::kApplicationStateConcealed) {
    layout_results.on_rasterized_callback.Run();
    return;
  }

  renderer::Submission renderer_submission(
      CreateSubmissionFromLayoutResults(layout_results));

  // Set the timeline id for the main web module.  The main web module is
  // assumed to be an interactive experience for which the default timeline
  // configuration is already designed for, so we don't configure anything
  // explicitly.
  renderer_submission.timeline_info.id = current_main_web_module_timeline_id_;

  renderer_submission.on_rasterized_callbacks.push_back(
      base::Bind(&BrowserModule::OnWebModuleRendererSubmissionRasterized,
                 base::Unretained(this)));

  if (!splash_screen_) {
    render_tree_combiner_.SetTimelineLayer(main_web_module_layer_.get());
  }
  main_web_module_layer_->Submit(renderer_submission);

  SubmitCurrentRenderTreeToRenderer();
}

void BrowserModule::OnSplashScreenRenderTreeProduced(
    const browser::WebModule::LayoutResults& layout_results) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnSplashScreenRenderTreeProduced()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());

  if (application_state_ == base::kApplicationStateConcealed ||
      !splash_screen_) {
    return;
  }

  renderer::Submission renderer_submission(
      CreateSubmissionFromLayoutResults(layout_results));

  // We customize some of the renderer pipeline timeline behavior to cater for
  // non-interactive splash screen playback.
  renderer_submission.timeline_info.id = current_splash_screen_timeline_id_;
  // Since the splash screen is non-interactive, latency is not a concern.
  // Latency reduction implies a speedup in animation playback speed which can
  // make the splash screen play out quicker than intended.
  renderer_submission.timeline_info.allow_latency_reduction = false;
  // Increase the submission queue size to a larger value than usual.  This
  // is done because a) since we do not attempt to reduce latency, the queue
  // tends to fill up more and b) the pipeline may end up receiving a number
  // of render tree submissions caused by updated main web module render trees,
  // which can fill the submission queue.  Blowing the submission queue is
  // particularly bad for the splash screen as it results in dropping of older
  // submissions, which results in skipping forward during animations, which
  // sucks.
  renderer_submission.timeline_info.max_submission_queue_size =
      std::max(8, renderer_submission.timeline_info.max_submission_queue_size);

  renderer_submission.on_rasterized_callbacks.push_back(base::Bind(
      &BrowserModule::OnRendererSubmissionRasterized, base::Unretained(this)));

  render_tree_combiner_.SetTimelineLayer(splash_screen_layer_.get());
  splash_screen_layer_->Submit(renderer_submission);

  // TODO: write screen shot using render_tree_combiner_ (to combine
  // splash screen and main web_module). Consider when the splash
  // screen is overlaid on top of the main web module render tree, and
  // a screenshot is taken : there will be a race condition on which
  // web module update their render tree last.

  SubmitCurrentRenderTreeToRenderer();
}

void BrowserModule::QueueOnQrCodeOverlayRenderTreeProduced(
    const scoped_refptr<render_tree::Node>& render_tree) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::QueueOnQrCodeOverlayRenderTreeProduced()");
  render_tree_submission_queue_.AddMessage(
      base::Bind(&BrowserModule::OnQrCodeOverlayRenderTreeProduced,
                 base::Unretained(this), render_tree));
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&BrowserModule::ProcessRenderTreeSubmissionQueue, weak_this_));
}

void BrowserModule::OnQrCodeOverlayRenderTreeProduced(
    const scoped_refptr<render_tree::Node>& render_tree) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnQrCodeOverlayRenderTreeProduced()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(qr_overlay_info_layer_);

  if (application_state_ == base::kApplicationStateConcealed) {
    return;
  }

  qr_overlay_info_layer_->Submit(renderer::Submission(render_tree));

  SubmitCurrentRenderTreeToRenderer();
}

void BrowserModule::OnWindowClose(base::TimeDelta close_time) {
#if defined(ENABLE_DEBUGGER)
  if (input_device_manager_fuzzer_) {
    return;
  }
#endif

  SbSystemRequestStop(0);
}

void BrowserModule::OnWindowMinimize() {
#if defined(ENABLE_DEBUGGER)
  if (input_device_manager_fuzzer_) {
    return;
  }
#endif
  SbSystemRequestConceal();
}

void BrowserModule::OnWindowSizeChanged(const ViewportSize& viewport_size) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (web_module_) {
    web_module_->SetSize(viewport_size);
  }
#if defined(ENABLE_DEBUGGER)
  if (debug_console_) {
    debug_console_->web_module().SetSize(viewport_size);
  }
#endif  // defined(ENABLE_DEBUGGER)
  if (splash_screen_) {
    splash_screen_->web_module().SetSize(viewport_size);
  }

  return;
}

void BrowserModule::OnOnScreenKeyboardShown(
    const base::OnScreenKeyboardShownEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // Only inject shown events to the main WebModule.
  on_screen_keyboard_show_called_ = true;
  if (splash_screen_ && splash_screen_->ShutdownSignaled()) {
    DestroySplashScreen();
  }
  if (web_module_) {
    web_module_->InjectOnScreenKeyboardShownEvent(event->ticket());
  }
}

void BrowserModule::OnOnScreenKeyboardHidden(
    const base::OnScreenKeyboardHiddenEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // Only inject hidden events to the main WebModule.
  if (web_module_) {
    web_module_->InjectOnScreenKeyboardHiddenEvent(event->ticket());
  }
}

void BrowserModule::OnOnScreenKeyboardFocused(
    const base::OnScreenKeyboardFocusedEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // Only inject focused events to the main WebModule.
  if (web_module_) {
    web_module_->InjectOnScreenKeyboardFocusedEvent(event->ticket());
  }
}

void BrowserModule::OnOnScreenKeyboardBlurred(
    const base::OnScreenKeyboardBlurredEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  // Only inject blurred events to the main WebModule.
  if (web_module_) {
    web_module_->InjectOnScreenKeyboardBlurredEvent(event->ticket());
  }
}

void BrowserModule::OnCaptionSettingsChanged(
    const base::AccessibilityCaptionSettingsChangedEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (web_module_) {
    web_module_->InjectCaptionSettingsChangedEvent();
  }
}

void BrowserModule::OnDateTimeConfigurationChanged(
    const base::DateTimeConfigurationChangedEvent* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  icu::TimeZone::adoptDefault(icu::TimeZone::detectHostTimeZone());
  if (web_module_) {
    web_module_->UpdateDateTimeConfiguration();
  }
}

#if defined(ENABLE_DEBUGGER)
void BrowserModule::OnFuzzerToggle(const std::string& message) {
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE, base::Bind(&BrowserModule::OnFuzzerToggle,
                                                 weak_this_, message));
    return;
  }

  if (!input_device_manager_fuzzer_) {
    // Wire up the input fuzzer key generator to the keyboard event callback.
    input_device_manager_fuzzer_ = std::unique_ptr<input::InputDeviceManager>(
        new input::InputDeviceManagerFuzzer(
            base::Bind(&BrowserModule::InjectKeyEventToMainWebModule,
                       base::Unretained(this))));
  } else {
    input_device_manager_fuzzer_.reset();
  }
}

void BrowserModule::OnDisableMediaCodecs(const std::string& codecs) {
  disabled_media_codecs_ = codecs;
  can_play_type_handler_->SetDisabledMediaCodecs(codecs);
}

void BrowserModule::QueueOnDebugConsoleRenderTreeProduced(
    const browser::WebModule::LayoutResults& layout_results) {
#if defined(ENABLE_DEBUGGER)
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::QueueOnDebugConsoleRenderTreeProduced()");
  render_tree_submission_queue_.AddMessage(
      base::Bind(&BrowserModule::OnDebugConsoleRenderTreeProduced,
                 base::Unretained(this), layout_results));
  task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&BrowserModule::ProcessRenderTreeSubmissionQueue, weak_this_));
#endif
}

void BrowserModule::OnDebugConsoleRenderTreeProduced(
    const browser::WebModule::LayoutResults& layout_results) {
#if defined(ENABLE_DEBUGGER)
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnDebugConsoleRenderTreeProduced()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (!debug_console_ ||
      (application_state_ == base::kApplicationStateConcealed)) {
    return;
  }

  if (!debug_console_->IsVisible()) {
    // If the layer already has no render tree then simply return. In that case
    // nothing is changing.
    if (!debug_console_layer_->HasRenderTree()) {
      return;
    }
    debug_console_layer_->Submit(base::nullopt);
  } else {
    debug_console_layer_->Submit(
        CreateSubmissionFromLayoutResults(layout_results));
  }

  SubmitCurrentRenderTreeToRenderer();
#endif
}

void BrowserModule::OnNavigateTimedTrace(const std::string& time) {
  double duration_in_seconds = 0;
  base::StringToDouble(time, &duration_in_seconds);
  navigate_timed_trace_duration_ =
      base::TimeDelta::FromMilliseconds(static_cast<int64_t>(
          duration_in_seconds * base::Time::kMillisecondsPerSecond));
}
#endif  // defined(ENABLE_DEBUGGER)

void BrowserModule::OnOnScreenKeyboardInputEventProduced(
    base_token::Token type, const dom::InputEventInit& event) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnOnScreenKeyboardInputEventProduced()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE,
        base::Bind(&BrowserModule::OnOnScreenKeyboardInputEventProduced,
                   weak_this_, type, event));
    return;
  }

#if defined(ENABLE_DEBUGGER)
  if (debug_console_ &&
      !debug_console_->FilterOnScreenKeyboardInputEvent(type, event)) {
    return;
  }
#endif  // defined(ENABLE_DEBUGGER)

  InjectOnScreenKeyboardInputEventToMainWebModule(type, event);
}

void BrowserModule::OnKeyEventProduced(base_token::Token type,
                                       const dom::KeyboardEventInit& event) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnKeyEventProduced()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::OnKeyEventProduced, weak_this_,
                              type, event));
    return;
  }

  // Filter the key event.
  if (!FilterKeyEvent(type, event)) {
    return;
  }

  InjectKeyEventToMainWebModule(type, event);
}

void BrowserModule::OnPointerEventProduced(base_token::Token type,
                                           const dom::PointerEventInit& event) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnPointerEventProduced()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&BrowserModule::OnPointerEventProduced,
                                      weak_this_, type, event));
    return;
  }

#if defined(ENABLE_DEBUGGER)
  if (debug_console_ && !debug_console_->FilterPointerEvent(type, event)) {
    return;
  }
#endif  // defined(ENABLE_DEBUGGER)

  scroll_engine_->thread()->task_runner()->PostTask(
      FROM_HERE,
      base::Bind(
          &ui_navigation::scroll_engine::ScrollEngine::HandlePointerEvent,
          base::Unretained(scroll_engine_.get()), type, event));

  DCHECK(web_module_);
  web_module_->InjectPointerEvent(type, event);
}

void BrowserModule::OnWheelEventProduced(base_token::Token type,
                                         const dom::WheelEventInit& event) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnWheelEventProduced()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::OnWheelEventProduced, weak_this_,
                              type, event));
    return;
  }

#if defined(ENABLE_DEBUGGER)
  if (debug_console_ && !debug_console_->FilterWheelEvent(type, event)) {
    return;
  }
#endif  // defined(ENABLE_DEBUGGER)

  DCHECK(web_module_);
  web_module_->InjectWheelEvent(type, event);
}

void BrowserModule::OnWindowOnOnlineEvent(const base::Event* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (web_module_) {
    web_module_->InjectWindowOnOnlineEvent(event);
  }
}

void BrowserModule::OnWindowOnOfflineEvent(const base::Event* event) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (web_module_) {
    web_module_->InjectWindowOnOfflineEvent(event);
  }
}

void BrowserModule::InjectKeyEventToMainWebModule(
    base_token::Token type, const dom::KeyboardEventInit& event) {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::InjectKeyEventToMainWebModule()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::InjectKeyEventToMainWebModule,
                              weak_this_, type, event));
    return;
  }

  DCHECK(web_module_);
  web_module_->InjectKeyboardEvent(type, event);
}

void BrowserModule::InjectOnScreenKeyboardInputEventToMainWebModule(
    base_token::Token type, const dom::InputEventInit& event) {
  TRACE_EVENT0(
      "cobalt::browser",
      "BrowserModule::InjectOnScreenKeyboardInputEventToMainWebModule()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE,
        base::Bind(
            &BrowserModule::InjectOnScreenKeyboardInputEventToMainWebModule,
            weak_this_, type, event));
    return;
  }

  DCHECK(web_module_);
  web_module_->InjectOnScreenKeyboardInputEvent(type, event);
}

void BrowserModule::OnError(const GURL& url, const std::string& error) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnError()");
  LOG(ERROR) << error;
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::OnError, weak_this_, url, error));
    return;
  }

  // Set |pending_navigate_url_| to the url where the error occurred. This will
  // cause the OnError callback to Navigate() to this URL if it receives a
  // positive response; otherwise, if Cobalt is currently preloaded or
  // suspended, then this is the url that Cobalt will navigate to when it starts
  // or resumes.
  pending_navigate_url_ = url;

  // Start the OnErrorRetry() timer if it isn't already running.
  // The minimum delay between calls to OnErrorRetry() exponentially grows as
  // |on_error_retry_count_| increases. |on_error_retry_count_| is reset when
  // OnLoad() is called.
  if (!on_error_retry_timer_.IsRunning()) {
    const int64 kBaseRetryDelayInMilliseconds = 1000;
    // Cap the max error shift at 10 (1024 * kBaseDelayInMilliseconds)
    // This results in the minimum delay being capped at ~17 minutes.
    const int kMaxOnErrorRetryCountShift = 10;
    int64 min_delay = kBaseRetryDelayInMilliseconds << std::min(
                          kMaxOnErrorRetryCountShift, on_error_retry_count_);
    int64 required_delay = std::max(
        min_delay -
            (base::TimeTicks::Now() - on_error_retry_time_).InMilliseconds(),
        static_cast<int64>(0));

    on_error_retry_timer_.Start(
        FROM_HERE, base::TimeDelta::FromMilliseconds(required_delay), this,
        &BrowserModule::OnErrorRetry);
  }
}

void BrowserModule::OnErrorRetry() {
  ++on_error_retry_count_;
  on_error_retry_time_ = base::TimeTicks::Now();
  waiting_for_error_retry_ = true;

  SystemPlatformErrorHandler::SystemPlatformErrorOptions options;
  options.error_type = kSbSystemPlatformErrorTypeConnectionError;
  options.callback =
      base::Bind(&BrowserModule::OnNetworkFailureSystemPlatformResponse,
                 base::Unretained(this));
  system_platform_error_handler_.RaiseSystemPlatformError(options);
}

void BrowserModule::OnNetworkFailureSystemPlatformResponse(
    SbSystemPlatformErrorResponse response) {
  // A positive response means we should retry, anything else we stop.
  if (response == kSbSystemPlatformErrorResponsePositive) {
    // We shouldn't be here if we don't have a pending URL from an error.
    DCHECK(pending_navigate_url_.is_valid());
    if (pending_navigate_url_.is_valid()) {
      Navigate(pending_navigate_url_);
    }
  } else {
    LOG(ERROR) << "Stop after network error";
    SbSystemRequestStop(0);
  }
}

bool BrowserModule::FilterKeyEvent(base_token::Token type,
                                   const dom::KeyboardEventInit& event) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::FilterKeyEvent()");
  // Check for hotkeys first. If it is a hotkey, no more processing is needed.
  // TODO: Let WebModule handle the event first, and let the web app prevent
  // this default behavior of the user agent.
  // https://developer.mozilla.org/en-US/docs/Web/API/Event/preventDefault
  if (!FilterKeyEventForHotkeys(type, event)) {
    return false;
  }

#if defined(ENABLE_DEBUGGER)
  if (debug_console_ && !debug_console_->FilterKeyEvent(type, event)) {
    return false;
  }
#endif  // defined(ENABLE_DEBUGGER)

  return true;
}

bool BrowserModule::FilterKeyEventForHotkeys(
    base_token::Token type, const dom::KeyboardEventInit& event) {
#if defined(ENABLE_DEBUGGER)
  if (debug_console_ &&
      (event.key_code() == dom::keycode::kF1 ||
       (event.ctrl_key() && event.key_code() == dom::keycode::kO))) {
    if (type == base::Tokens::keydown()) {
      // F1 or Ctrl+O cycles the debug console display.
      debug_console_->CycleMode();
    }
    return false;
  } else if (event.key_code() == dom::keycode::kF5) {
    if (type == base::Tokens::keydown()) {
      // F5 reloads the page.
      Reload();
    }
  } else if (event.ctrl_key() && event.key_code() == dom::keycode::kS) {
    if (type == base::Tokens::keydown()) {
      SbSystemRequestConceal();
    }
    return false;
  }
#endif  // defined(ENABLE_DEBUGGER)

  return true;
}

void BrowserModule::AddURLHandler(
    const URLHandler::URLHandlerCallback& callback) {
  url_handlers_.push_back(callback);
}

void BrowserModule::RemoveURLHandler(
    const URLHandler::URLHandlerCallback& callback) {
  for (URLHandlerCollection::iterator iter = url_handlers_.begin();
       iter != url_handlers_.end(); ++iter) {
    if (*iter == callback) {
      url_handlers_.erase(iter);
      return;
    }
  }
}

bool BrowserModule::NavigateTryURLHandlers(const GURL& url) {
  for (URLHandlerCollection::const_iterator iter = url_handlers_.begin();
       iter != url_handlers_.end(); ++iter) {
    if (iter->Run(url)) {
      return true;
    }
  }

  // No registered handler handled the URL, let the caller handle it.
  return false;
}

void BrowserModule::DestroySplashScreen(base::TimeDelta close_time) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::DestroySplashScreen()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::DestroySplashScreen, weak_this_,
                              close_time));
    return;
  }
  if (splash_screen_) {
    lifecycle_observers_.RemoveObserver(splash_screen_.get());
    if (!close_time.is_zero() && renderer_module_) {
      // Ensure that the renderer renders each frame up until the window.close()
      // is called on the splash screen's timeline, in order to ensure that the
      // splash screen shutdown transition plays out completely.
      renderer_module_->pipeline()->TimeFence(close_time);
    }
    splash_screen_layer_->Reset();
    SubmitCurrentRenderTreeToRenderer();
    splash_screen_.reset();
  }
}

void BrowserModule::DestroyScrollEngine() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::DestroyScrollEngine()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(
        FROM_HERE, base::Bind(&BrowserModule::DestroyScrollEngine, weak_this_));
    return;
  }
  if (scroll_engine_ &&
      lifecycle_observers_.HasObserver(scroll_engine_.get())) {
    lifecycle_observers_.RemoveObserver(scroll_engine_.get());
  }
  scroll_engine_.reset();
}

#if defined(ENABLE_WEBDRIVER)
std::unique_ptr<webdriver::SessionDriver> BrowserModule::CreateSessionDriver(
    const webdriver::protocol::SessionId& session_id) {
  return base::WrapUnique(new webdriver::SessionDriver(
      session_id,
      base::Bind(&BrowserModule::CreateWindowDriver, base::Unretained(this)),
      base::Bind(&BrowserModule::WaitForLoad, base::Unretained(this))));
}

std::unique_ptr<webdriver::WindowDriver> BrowserModule::CreateWindowDriver(
    const webdriver::protocol::WindowId& window_id) {
  // Repost to our task runner to ensure synchronous access to |web_module_|.
  std::unique_ptr<webdriver::WindowDriver> window_driver;
  base::task_runner_util::PostBlockingTask(
      task_runner_, FROM_HERE,
      base::BindOnce(&BrowserModule::CreateWindowDriverInternal,
                     base::Unretained(this), window_id,
                     base::Unretained(&window_driver)));

  // This log is relied on by the webdriver benchmark tests, so it shouldn't be
  // changed unless the corresponding benchmark logic is changed as well.
  LOG(INFO) << "Created WindowDriver: ID=" << window_id.id();
  DCHECK(window_driver);
  return window_driver;
}

void BrowserModule::CreateWindowDriverInternal(
    const webdriver::protocol::WindowId& window_id,
    std::unique_ptr<webdriver::WindowDriver>* out_window_driver) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(web_module_);
  web_module_->CreateWindowDriver(window_id, out_window_driver);
}
#endif  // defined(ENABLE_WEBDRIVER)

#if defined(ENABLE_DEBUGGER)
std::unique_ptr<debug::DebugClient> BrowserModule::CreateDebugClient(
    debug::DebugClient::Delegate* delegate) {
  // Repost to our task runner to ensure synchronous access to |web_module_|.
  debug::backend::DebugDispatcher* debug_dispatcher = nullptr;
  base::task_runner_util::PostBlockingTask(
      task_runner_, FROM_HERE,
      base::BindOnce(&BrowserModule::GetDebugDispatcherInternal,
                     base::Unretained(this),
                     base::Unretained(&debug_dispatcher)));
  if (debug_dispatcher) {
    return std::unique_ptr<debug::DebugClient>(
        new debug::DebugClient(debug_dispatcher, delegate));
  } else {
    LOG(ERROR)
        << "Debugger connected but debugging the main web module is disabled.";
    return nullptr;
  }
}

void BrowserModule::GetDebugDispatcherInternal(
    debug::backend::DebugDispatcher** out_debug_dispatcher) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(web_module_);
  web_module_->GetDebugDispatcher(out_debug_dispatcher);
}
#endif  // ENABLE_DEBUGGER

void BrowserModule::SetProxy(const std::string& proxy_rules) {
  // NetworkModule will ensure this happens on the correct thread.
  network_module_->SetProxy(proxy_rules);
}

void BrowserModule::Blur(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Blur()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateStarted);
  application_state_ = base::kApplicationStateBlurred;
  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_, Blur(timestamp));

  // The window is about to lose focus, and may be destroyed.
  if (media_module_) {
    DCHECK(system_window_);
    window_size_ = system_window_->GetWindowSize();
  }
}

void BrowserModule::Conceal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Conceal()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateBlurred);
  application_state_ = base::kApplicationStateConcealed;
  ConcealInternal(timestamp);
}

void BrowserModule::Freeze(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Freeze()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateConcealed);
  application_state_ = base::kApplicationStateFrozen;
  FreezeInternal(timestamp);
}

void BrowserModule::Unfreeze(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Unfreeze()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateFrozen);
  application_state_ = base::kApplicationStateConcealed;
  UnfreezeInternal(timestamp);
  NavigatePendingURL();
}

void BrowserModule::Reveal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Reveal()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateConcealed);
  application_state_ = base::kApplicationStateBlurred;
  RevealInternal(timestamp);
}

void BrowserModule::Focus(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::Focus()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(application_state_ == base::kApplicationStateBlurred);
  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_, Focus(timestamp));
  application_state_ = base::kApplicationStateStarted;
}

base::ApplicationState BrowserModule::GetApplicationState() {
  return application_state_;
}

void BrowserModule::ReduceMemory() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  if (splash_screen_) {
    splash_screen_->ReduceMemory();
  }

#if defined(ENABLE_DEBUGGER)
  if (debug_console_) {
    debug_console_->ReduceMemory();
  }
#endif  // defined(ENABLE_DEBUGGER)

  if (web_module_) {
    web_module_->ReduceMemory();
  }
}

void BrowserModule::CheckMemory(
    const int64_t& used_cpu_memory,
    const base::Optional<int64_t>& used_gpu_memory) {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  memory_settings_checker_.RunChecks(auto_mem_, used_cpu_memory,
                                     used_gpu_memory);
}

void BrowserModule::UpdateJavaScriptHeapStatistics() {
  web_module_->RequestJavaScriptHeapStatistics(base::Bind(
      &BrowserModule::GetHeapStatisticsCallback, base::Unretained(this)));
}

void BrowserModule::OnRendererSubmissionRasterized() {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnRendererSubmissionRasterized()");
  if (!is_rendered_) {
    // Hide the system splash screen when the first render has completed.
    is_rendered_ = true;
    SbSystemHideSplashScreen();
  }
}

void BrowserModule::OnWebModuleRendererSubmissionRasterized() {
  TRACE_EVENT0("cobalt::browser",
               "BrowserModule::OnFirstWebModuleSubmissionRasterized()");
  OnRendererSubmissionRasterized();
  if (!is_web_module_rendered_) {
    is_web_module_rendered_ = true;
    const CobaltExtensionGraphicsApi* graphics_extension =
        static_cast<const CobaltExtensionGraphicsApi*>(
            SbSystemGetExtension(kCobaltExtensionGraphicsName));
    if (graphics_extension &&
        strcmp(graphics_extension->name, kCobaltExtensionGraphicsName) == 0 &&
        graphics_extension->version >= 6) {
      graphics_extension->ReportFullyDrawn();
    }
  }
}

render_tree::ResourceProvider* BrowserModule::GetResourceProvider() {
  if (application_state_ == base::kApplicationStateConcealed) {
    DCHECK(resource_provider_stub_);
    return &(resource_provider_stub_.value());
  }

  if (renderer_module_) {
    return renderer_module_->resource_provider();
  }

  return nullptr;
}

void BrowserModule::InitializeComponents() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::InitializeComponents()");
  InstantiateRendererModule();

  if (media_module_) {
    media_module_->UpdateSystemWindowAndResourceProvider(system_window_.get(),
                                                         GetResourceProvider());
  } else {
    options_.media_module_options.allow_resume_after_suspend =
        SbSystemSupportsResume();
    options_.media_module_options.persistent_settings =
        options_.persistent_settings;
    media_module_.reset(new media::MediaModule(system_window_.get(),
                                               GetResourceProvider(),
                                               options_.media_module_options));

    if (web_module_) {
      web_module_->SetMediaModule(media_module_.get());
    }
  }

  if (web_module_) {
    web_module_->UpdateCamera3D(input_device_manager_->camera_3d());
    web_module_->GetUiNavRoot()->SetContainerWindow(
        system_window_->GetSbWindow());
  }
}

void BrowserModule::InitializeSystemWindow() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::InitializeSystemWindow()");
  DCHECK(!system_window_);
  if (media_module_ && !window_size_.IsEmpty()) {
    system_window_.reset(
        new system_window::SystemWindow(event_dispatcher_, window_size_));
  } else {
    base::Optional<math::Size> maybe_size;
    if (options_.requested_viewport_size) {
      maybe_size = options_.requested_viewport_size->width_height();
    }
    system_window_.reset(
        new system_window::SystemWindow(event_dispatcher_, maybe_size));

    // Reapply automem settings now that we may have a different viewport size.
    ApplyAutoMemSettings();
  }

  input_device_manager_ = input::InputDeviceManager::CreateFromWindow(
      base::Bind(&BrowserModule::OnKeyEventProduced, base::Unretained(this)),
      base::Bind(&BrowserModule::OnPointerEventProduced,
                 base::Unretained(this)),
      base::Bind(&BrowserModule::OnWheelEventProduced, base::Unretained(this)),
      base::Bind(&BrowserModule::OnOnScreenKeyboardInputEventProduced,
                 base::Unretained(this)),
      system_window_.get());

  InitializeComponents();
}

void BrowserModule::InstantiateRendererModule() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::InstantiateRendererModule()");
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(system_window_);
  DCHECK(!renderer_module_);

  renderer_module_.reset(new renderer::RendererModule(
      system_window_.get(),
      RendererModuleWithCameraOptions(options_.renderer_module_options,
                                      input_device_manager_->camera_3d())));
  screen_shot_writer_.reset();
}

void BrowserModule::DestroyRendererModule() {
  DCHECK(task_runner_->RunsTasksInCurrentSequence());
  DCHECK(renderer_module_);
  base::task_runner_util::PostBlockingTask(
      web_module_->task_runner(), FROM_HERE,
      base::Bind(&WebModule::WaitForItall,
                 base::Unretained(web_module_.get())));
  screen_shot_writer_.reset();
  renderer_module_.reset();
}

void BrowserModule::FreezeMediaModule() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::FreezeMediaModule()");
  if (media_module_) {
    media_module_->Suspend();
  }
}

void BrowserModule::NavigatePendingURL() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::NavigatePendingURL()");
  // If there's a navigation that's pending, then attempt to navigate to its
  // specified URL now, unless we're still waiting for an error retry.
  if (pending_navigate_url_.is_valid() && !waiting_for_error_retry_) {
    Navigate(pending_navigate_url_);
  }
}

void BrowserModule::ResetResources() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::ResetResources()");
  if (qr_code_overlay_) {
    qr_code_overlay_->SetResourceProvider(NULL);
  }

  // Flush out any submitted render trees pushed since we started shutting down
  // the web modules above.
  render_tree_submission_queue_.ProcessAll();

  // Clear out the render tree combiner so that it doesn't hold on to any
  // render tree resources either.
  main_web_module_layer_->Reset();
  splash_screen_layer_->Reset();
#if defined(ENABLE_DEBUGGER)
  if (debug_console_layer_) {
    debug_console_layer_->Reset();
  }
#endif  // defined(ENABLE_DEBUGGER)
  if (qr_overlay_info_layer_) {
    qr_overlay_info_layer_->Reset();
  }
}

void BrowserModule::UpdateScreenSize() {
  ViewportSize size = GetViewportSize();
#if defined(ENABLE_DEBUGGER)
  if (debug_console_) {
    debug_console_->SetSize(size);
  }
#endif  // defined(ENABLE_DEBUGGER)

  if (splash_screen_) {
    splash_screen_->SetSize(size);
  }

  if (web_module_) {
    web_module_->SetSize(size);
  }

  if (qr_code_overlay_) {
    qr_code_overlay_->SetSize(size.width_height());
  }
}

void BrowserModule::ConcealInternal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::ConcealInternal()");
  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_,
                    Conceal(GetResourceProvider(), timestamp));

  ResetResources();

  // Suspend media module and update resource provider.
  if (media_module_) {
    // This needs to be done before destroying the renderer module as it
    // may use the renderer module to release assets during the update.
    media_module_->UpdateSystemWindowAndResourceProvider(NULL,
                                                         GetResourceProvider());
  }

  if (renderer_module_) {
    // Destroy the renderer module into so that it releases all its graphical
    // resources.
    DestroyRendererModule();
  }

  // Reset system window after renderer module destroyed.
  if (media_module_) {
    input_device_manager_.reset();
    system_window_.reset();
  }
}

void BrowserModule::FreezeInternal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::FreezeInternal()");
  FreezeMediaModule();
  // First freeze all our web modules which implies that they will release
  // their resource provider and all resources created through it.
  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_, Freeze(timestamp));

  if (network_module_) {
    // Synchronously wait for storage to flush before returning from freezing.
    network_module_->storage_manager()->FlushSynchronous();
  }
}

void BrowserModule::RevealInternal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::RevealInternal()");
  DCHECK(!renderer_module_);
  if (!system_window_) {
    InitializeSystemWindow();
  } else {
    InitializeComponents();
  }

  DCHECK(system_window_);
  window_size_ = system_window_->GetWindowSize();

  // Propagate the current screen size.
  UpdateScreenSize();

  // Set resource provider right after render module initialized.
  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_,
                    Reveal(GetResourceProvider(), timestamp));

  if (qr_code_overlay_) {
    qr_code_overlay_->SetResourceProvider(GetResourceProvider());
  }
}

void BrowserModule::UnfreezeInternal(int64_t timestamp) {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::UnfreezeInternal()");
  // Set the Stub resource provider to media module and to web module
  // at Concealed state.
  if (media_module_) media_module_->Resume(GetResourceProvider());

  FOR_EACH_OBSERVER(LifecycleObserver, lifecycle_observers_,
                    Unfreeze(GetResourceProvider(), timestamp));
#if defined(DIAL_SERVER)
  if (network_module_) {
    network_module_->RestartDialService();
  }
#endif
}

void BrowserModule::OnMaybeFreeze() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::OnMaybeFreeze()");
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE, base::Bind(&BrowserModule::OnMaybeFreeze,
                                                 base::Unretained(this)));
    return;
  }

  bool splash_screen_ready_to_freeze =
      splash_screen_ ? splash_screen_->IsReadyToFreeze() : true;
#if defined(ENABLE_DEBUGGER)
  bool debug_console_ready_to_freeze =
      debug_console_ ? debug_console_->IsReadyToFreeze() : true;
#endif  // defined(ENABLE_DEBUGGER)
  bool web_module_ready_to_freeze = web_module_->IsReadyToFreeze();
  if (splash_screen_ready_to_freeze &&
#if defined(ENABLE_DEBUGGER)
      debug_console_ready_to_freeze &&
#endif  // defined(ENABLE_DEBUGGER)
      web_module_ready_to_freeze &&
      application_state_ == base::kApplicationStateConcealed) {
    DLOG(INFO) << "System request to freeze the app.";
    SbSystemRequestFreeze();
  }
}

ViewportSize BrowserModule::GetViewportSize() {
  // If a custom render root transform is used, report the size of the
  // transformed viewport.
  math::Matrix3F viewport_transform = math::Matrix3F::Identity();
  static const CobaltExtensionGraphicsApi* s_graphics_extension =
      static_cast<const CobaltExtensionGraphicsApi*>(
          SbSystemGetExtension(kCobaltExtensionGraphicsName));
  float m00, m01, m02, m10, m11, m12, m20, m21, m22;
  if (s_graphics_extension &&
      strcmp(s_graphics_extension->name, kCobaltExtensionGraphicsName) == 0 &&
      s_graphics_extension->version >= 5 &&
      s_graphics_extension->GetRenderRootTransform(&m00, &m01, &m02, &m10, &m11,
                                                   &m12, &m20, &m21, &m22)) {
    viewport_transform =
        math::Matrix3F::FromValues(m00, m01, m02, m10, m11, m12, m20, m21, m22);
  }

  // We trust the renderer module for width and height the most, if it exists.
  if (renderer_module_) {
    math::Size target_size = renderer_module_->render_target_size();
    // ...but get the diagonal from one of the other modules.
    float diagonal_inches = 0;
    float device_pixel_ratio = 1.0f;
    if (system_window_) {
      diagonal_inches = system_window_->GetDiagonalSizeInches();
      device_pixel_ratio = system_window_->GetDevicePixelRatio();

      // For those platforms that can have a main window size smaller than the
      // render target size, the system_window_ size (if exists) should be
      // trusted over the renderer_module_ render target size.
      math::Size window_size = system_window_->GetWindowSize();
      if (window_size.width() <= target_size.width() &&
          window_size.height() <= target_size.height()) {
        target_size = window_size;
      }
    } else if (options_.requested_viewport_size) {
      diagonal_inches = options_.requested_viewport_size->diagonal_inches();
      device_pixel_ratio =
          options_.requested_viewport_size->device_pixel_ratio();
    }

    target_size =
        math::RoundOut(viewport_transform.MapRect(math::RectF(target_size)))
            .size();
    ViewportSize v(target_size.width(), target_size.height(), diagonal_inches,
                   device_pixel_ratio);
    return v;
  }

  // If the system window exists, that's almost just as good.
  if (system_window_) {
    math::Size size = system_window_->GetWindowSize();
    size = math::RoundOut(viewport_transform.MapRect(math::RectF(size))).size();
    ViewportSize v(size.width(), size.height(),
                   system_window_->GetDiagonalSizeInches(),
                   system_window_->GetDevicePixelRatio());
    return v;
  }

  // Otherwise, we assume we'll get the viewport size that was requested.
  if (options_.requested_viewport_size) {
    math::Size requested_size =
        math::RoundOut(viewport_transform.MapRect(math::RectF(
                           options_.requested_viewport_size->width(),
                           options_.requested_viewport_size->height())))
            .size();
    return ViewportSize(requested_size.width(), requested_size.height(),
                        options_.requested_viewport_size->diagonal_inches(),
                        options_.requested_viewport_size->device_pixel_ratio());
  }

  // TODO: Allow platforms to define the default window size and return that
  // here.

  // No window and no viewport size was requested, so we return a conservative
  // default.
  math::Size default_size(1920, 1080);
  default_size =
      math::RoundOut(viewport_transform.MapRect(math::RectF(default_size)))
          .size();
  ViewportSize view_size(default_size.width(), default_size.height());
  return view_size;
}

void BrowserModule::ApplyAutoMemSettings() {
  TRACE_EVENT0("cobalt::browser", "BrowserModule::ApplyAutoMemSettings()");
  auto_mem_.ConstructSettings(
      GetViewportSize().width_height(), options_.command_line_auto_mem_settings,
      options_.config_api_auto_mem_settings, enable_skia_rasterizer_);

  // Web Module options.
  options_.web_module_options.encoded_image_cache_capacity =
      static_cast<int>(auto_mem_.encoded_image_cache_size_in_bytes()->value());
  options_.web_module_options.image_cache_capacity =
      static_cast<int>(auto_mem_.image_cache_size_in_bytes()->value());
  options_.web_module_options.remote_typeface_cache_capacity = static_cast<int>(
      auto_mem_.remote_typeface_cache_size_in_bytes()->value());
  if (web_module_) {
    web_module_->SetImageCacheCapacity(
        auto_mem_.image_cache_size_in_bytes()->value());
    web_module_->SetRemoteTypefaceCacheCapacity(
        auto_mem_.remote_typeface_cache_size_in_bytes()->value());
  }

  // Renderer Module options.
  options_.renderer_module_options.skia_cache_size_in_bytes =
      static_cast<int>(auto_mem_.skia_cache_size_in_bytes()->value());
  options_.renderer_module_options.offscreen_target_cache_size_in_bytes =
      static_cast<int>(
          auto_mem_.offscreen_target_cache_size_in_bytes()->value());

  const memory_settings::TextureDimensions skia_glyph_atlas_texture_dimensions =
      auto_mem_.skia_atlas_texture_dimensions()->value();
  if (skia_glyph_atlas_texture_dimensions.bytes_per_pixel() > 0) {
    // Right now the bytes_per_pixel is assumed in the engine. Any other value
    // is currently forbidden.
    DCHECK_EQ(2, skia_glyph_atlas_texture_dimensions.bytes_per_pixel());

    options_.renderer_module_options.skia_glyph_texture_atlas_dimensions =
        math::Size(skia_glyph_atlas_texture_dimensions.width(),
                   skia_glyph_atlas_texture_dimensions.height());
  }
}

void BrowserModule::GetHeapStatisticsCallback(
    const script::HeapStatistics& heap_statistics) {
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&BrowserModule::GetHeapStatisticsCallback,
                                      base::Unretained(this), heap_statistics));
    return;
  }
  javascript_reserved_memory_ = heap_statistics.total_heap_size;
}

void BrowserModule::SubmitCurrentRenderTreeToRenderer() {
  if (web_module_) {
    web_module_->GetUiNavRoot()->PerformQueuedUpdates();
  }

  if (!renderer_module_) {
    return;
  }

  base::Optional<renderer::Submission> submission =
      render_tree_combiner_.GetCurrentSubmission();
  if (submission) {
    renderer_module_->pipeline()->Submit(*submission);
  }
}

SbWindow BrowserModule::GetSbWindow() {
  if (!system_window_) {
    return NULL;
  }
  return system_window_->GetSbWindow();
}

base::Optional<std::string> BrowserModule::SetSplashScreenTopicFallback(
    const GURL& url) {
  std::map<std::string, std::string> url_param_map;
  // If this is the initial startup, use topic within deeplink, if specified.
  if (main_web_module_generation_ == 1) {
    std::string deeplink = GetInitialDeepLink();
    size_t query_pos = deeplink.find('?');
    if (query_pos != std::string::npos) {
      GetParamMap(deeplink.substr(query_pos + 1), url_param_map);
    }
  }
  // If this is not the initial startup, there was no deeplink specified, or
  // the deeplink did not have a topic, check the current url for a topic.
  if (url_param_map["topic"].empty()) {
    GetParamMap(url.query(), url_param_map);
  }
  std::string splash_topic = url_param_map["topic"];
  // If a topic was found, check whether a fallback url was specified.
  if (!splash_topic.empty()) {
    GURL splash_url = options_.fallback_splash_screen_topic_map[splash_topic];
    if (!splash_url.spec().empty()) {
      // Update fallback splash screen url to topic-specific URL.
      fallback_splash_screen_url_ = splash_url;
    }
    return base::Optional<std::string>(splash_topic);
  }
  return base::Optional<std::string>();
}

void BrowserModule::GetParamMap(const std::string& url,
                                std::map<std::string, std::string>& map) {
  bool next_is_option = true;
  bool next_is_value = false;
  std::string option = "";
  base::StringTokenizer tokenizer(url, "&=");
  tokenizer.set_options(base::StringTokenizer::RETURN_DELIMS);

  while (tokenizer.GetNext()) {
    if (tokenizer.token_is_delim()) {
      switch (*tokenizer.token_begin()) {
        case '&':
          next_is_option = true;
          break;
        case '=':
          next_is_value = true;
          break;
      }
    } else {
      std::string token = tokenizer.token();
      if (next_is_value && !option.empty()) {
        // Overwrite previous value when an option appears more than once.
        map[option] = token;
      }
      option = "";
      if (next_is_option) {
        option = token;
      }
      next_is_option = false;
      next_is_value = false;
    }
  }
}

scoped_refptr<script::Wrappable> BrowserModule::CreateH5vccCallback(
    WebModule* web_module, web::EnvironmentSettings* settings) {
  DCHECK(!task_runner_->RunsTasksInCurrentSequence());
  // Note: This is a callback function that runs in the MainWebModule thread.
  // The web_module_ member can not be safely used in this function.

  h5vcc::H5vcc::Settings h5vcc_settings;
  h5vcc_settings.set_web_setting_func =
      base::Bind(&web::WebSettingsImpl::Set, base::Unretained(&web_settings_));
  h5vcc_settings.media_module = media_module_.get();
  h5vcc_settings.network_module = network_module_;
#if SB_IS(EVERGREEN)
  h5vcc_settings.updater_module = updater_module_;
#endif
  h5vcc_settings.event_dispatcher = event_dispatcher_;

  h5vcc_settings.user_agent_data = settings->context()
                                       ->GetWindowOrWorkerGlobalScope()
                                       ->navigator_base()
                                       ->user_agent_data();
  h5vcc_settings.global_environment = settings->context()->global_environment();
  h5vcc_settings.persistent_settings = options_.persistent_settings;
  h5vcc_settings.can_play_type_handler = can_play_type_handler_.get();

  auto* h5vcc_object = new h5vcc::H5vcc(h5vcc_settings);
  if (!web_module_created_callback_.is_null()) {
    web_module_created_callback_.Run(web_module);
  }
  return scoped_refptr<script::Wrappable>(h5vcc_object);
}

void BrowserModule::SetDeepLinkTimestamp(int64_t timestamp) {
  if (!task_runner_->RunsTasksInCurrentSequence()) {
    task_runner_->PostTask(FROM_HERE,
                           base::Bind(&BrowserModule::SetDeepLinkTimestamp,
                                      base::Unretained(this), timestamp));
    return;
  }
  DCHECK(web_module_);
  web_module_->SetDeepLinkTimestamp(timestamp);
}

void BrowserModule::ValidateCacheBackendSettings() {
  DCHECK(network_module_);
  auto url_request_context = network_module_->url_request_context();
  auto http_cache = url_request_context->http_transaction_factory()->GetCache();
  if (!http_cache) return;
  network_module_->url_request_context()->ValidateCachePersistentSettings();
}

#if defined(ENABLE_DEBUGGER)
std::string BrowserModule::OnBoxDumpMessage(const std::string& message) {
  std::string response = "No MainWebModule.";
  if (web_module_) {
    response = web_module_->OnBoxDumpMessage(message);
  }
  return response;
}
#endif

}  // namespace browser
}  // namespace cobalt
