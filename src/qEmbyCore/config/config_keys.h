#ifndef CONFIG_KEYS_H
#define CONFIG_KEYS_H

#include <QString>

namespace ConfigKeys {


inline QString forServer(const QString& serverId, const char* baseKey) {
    return QStringLiteral("server/%1/%2").arg(serverId, baseKey);
}


inline QString forLibrary(const QString& serverId, const QString& libraryId, const char* baseKey) {
    return QStringLiteral("server/%1/library/%2/%3").arg(serverId, libraryId, baseKey);
}


inline QString forCategory(const QString& serverId, const QString& categoryId, const char* baseKey) {
    return QStringLiteral("server/%1/category/%2/%3").arg(serverId, categoryId, baseKey);
}


inline QString forServerMedia(const QString& serverId, const QString& mediaId, const char* baseKey) {
    return QStringLiteral("%1/%2/%3").arg(QString::fromLatin1(baseKey), serverId, mediaId);
}


inline QString forServerMediaSource(const QString& serverId, const QString& mediaId,
                                    const QString& sourceId, const char* baseKey) {
    return QStringLiteral("%1/%2/%3/%4")
        .arg(QString::fromLatin1(baseKey), serverId, mediaId, sourceId);
}



constexpr const char* Language = "general/language";
constexpr const char* RememberServer = "general/remember_server";
constexpr const char* LastSelectedServerId = "general/last_selected_server_id";
constexpr const char* CloseToTray = "general/close_to_tray";
constexpr const char* SingleApplication = "general/single_application";
constexpr const char* CheckForUpdates = "general/check_for_updates";
constexpr const char* IgnoredUpdateVersion = "general/ignored_update_version";
constexpr const char* LogEnable = "general/log_enable";
constexpr const char* ApiTimeout = "general/api_timeout";
constexpr const char* ImageCacheLimit = "general/image_cache_limit";




constexpr const char* ProxyMode             = "network/proxy_mode";
constexpr const char* ProxyType             = "network/proxy_type";
constexpr const char* ProxyHost             = "network/proxy_host";
constexpr const char* ProxyPort             = "network/proxy_port";
constexpr const char* ProxyUsername         = "network/proxy_username";
constexpr const char* ProxyPassword         = "network/proxy_password";
constexpr const char* ProxyBypassLocalhost  = "network/proxy_bypass_local";

// Global default User-Agent for Emby API/streaming requests.
// Per-server ServerProfile::customUserAgent takes precedence over this.
constexpr const char* CustomUserAgent = "network/custom_user_agent";




constexpr const char* ThemeMode = "appearance/theme_mode";
constexpr const char* DefaultLibraryView = "appearance/default_library_view";
constexpr const char* FontSize = "appearance/font_size";
constexpr const char* SidebarPosition = "appearance/sidebar_position";
constexpr const char* SidebarPinned = "appearance/sidebar_pinned";
constexpr const char* SidebarCustomEnabled = "appearance/sidebar_custom_enabled";
constexpr const char* SidebarHideSearch = "appearance/sidebar_hide_search";
constexpr const char* SidebarHideHome = "appearance/sidebar_hide_home";
constexpr const char* SidebarHideFavorites = "appearance/sidebar_hide_favorites";
constexpr const char* StartupWindowState = "appearance/startup_window_state";
constexpr const char* UiAnimations = "appearance/ui_animations";
constexpr const char* SnapshotNavigation = "appearance/snapshot_navigation";
// 设置页导航记忆：上次离开设置页时停留的页签与该页签的纵向滚动位置
constexpr const char* SettingsLastTab = "appearance/settings_last_tab";       // int, 页签索引
constexpr const char* SettingsLastScroll = "appearance/settings_last_scroll"; // int, 纵向滚动值
constexpr const char* SearchHistoryEnabled = "appearance/search_history_enabled";
constexpr const char* SearchAutocompleteEnabled = "appearance/search_autocomplete_enabled";
constexpr const char* ShowRecommended = "library/show_recommended";
constexpr const char* ShowContinueWatching = "library/show_continue_watching";
constexpr const char* ShowLatestAdded = "library/show_latest_added";
constexpr const char* ShowCompletedWatching = "library/show_completed_watching";
constexpr const char* ContinueWatchingRequestLimit =
    "library/continue_watching_request_limit";
constexpr const char* LatestMediaRequestLimit =
    "library/latest_media_request_limit";
constexpr const char* RecommendedRequestLimit =
    "library/recommended_request_limit";
constexpr const char* CompletedWatchingRequestLimit =
    "library/completed_watching_request_limit";
constexpr const char* ShowMediaLibraries = "library/show_media_libraries";
constexpr const char* ShowEachLibrary = "library/show_each_library";
constexpr const char* ShowFavoriteFolders = "library/show_favorite_folders";
constexpr const char* CustomHomeSectionOrderEnabled =
    "library/custom_home_section_order_enabled";
constexpr const char* HomeSectionOrder = "library/home_section_order";
constexpr const char* ImageQuality = "library/image_quality";
constexpr const char* ShimmerAnimation = "library/shimmer_animation";
constexpr const char* AdaptiveImages = "library/adaptive_images";
constexpr const char* ShowMediaTooltips = "library/show_media_tooltips";

// 该服务器是否参与聚合（聚合搜索 / 聚合历史 / 聚合收藏）。
// per-server 键：server/<id>/aggregate_enabled；无键 = true（默认参与）。
constexpr const char* AggregateEnabled = "aggregate_enabled";


constexpr const char* LibrarySortIndex = "sort/index";
constexpr const char* LibrarySortDescending = "sort/descending";
constexpr const char* LibraryViewMode = "view/mode";


constexpr const char* CategoryViewMode = "view/mode";




constexpr const char* DataCacheDuration = "cache/data_cache_duration";
constexpr const char* ImageCacheDuration = "cache/image_cache_duration";




constexpr const char* PlayerHwDec = "player/hw_decoding";
constexpr const char* PlayerVideoSync = "player/video_sync";
constexpr const char* PlayerDefaultScale = "player/default_scale";
constexpr const char* PlayerAudioLang = "player/audio_lang";
constexpr const char* PlayerSubLang = "player/sub_lang";
constexpr const char* PlayerPreferredVersion = "player/preferred_version";

constexpr const char* PlayerSelectedMediaSource = "player/selected_media_source";

constexpr const char* PlayerSelectedAudioStream = "player/selected_audio_stream";
constexpr const char* PlayerSelectedSubtitleStream = "player/selected_subtitle_stream";
constexpr const char* PlayerVolNormal = "player/vol_normalization";
constexpr const char* PlayerContinuousPlay = "player/continuous_play";
constexpr const char* PlayerFastStart = "player/fast_start";
// Next-episode playback-source prefetch trigger: 0 = disabled, otherwise
// prefetch starts once playback progress reaches this percentage.
constexpr const char* PlayerPrefetchThreshold = "player/prefetch_threshold";
// Disk-backed demuxer cache: when enabled, mpv writes the demuxer cache's
// packet data to disk instead of RAM (--cache-on-disk), which is what makes a
// very large demuxer-max-bytes affordable without the same cost in memory. The
// target directory comes from --demuxer-cache-dir. The cache lives and dies
// with the demuxer, so this is NOT a persistent cache and does not make
// replaying the same media faster.
//
// NOTE for anyone wondering why the configured directory looks empty while
// playback is running: that is mpv's intended behaviour. demux_cache_create()
// creates "mpv-cache-XXXXXX.dat" in that directory and then immediately
// unlinks it (--demuxer-cache-unlink-files defaults to "immediate"). The file
// still occupies disk space and its descriptor stays readable/writable, but it
// has no directory entry any more, so Explorer shows nothing. Set
// demuxer-cache-unlink-files to "whendone" or "no" to keep the file visible.
constexpr const char* PlayerDiskCache = "player/disk_cache";
constexpr const char* PlayerDiskCacheDir = "player/disk_cache_dir";
// Local HTTP relay in front of mpv (MpvHttpStreamRelay). Disabled by default:
// the sources it helps are the minority (audio track stored apart from the
// video, i.e. a non-interleaved layout) and for a normally interleaved file
// the relay is pure overhead -- an extra local hop plus its own thread for a
// request pattern that was already sequential. Turning it on keeps a
// byte-range cache so those odd sources do not pay a network round trip for
// every audio chunk, at the cost of a local proxy that can be seen burning a
// core while it serves the resulting request storm. When this is false the
// relay is not merely idling: it is never created, so mpv talks to the
// original upstream URL with no local hop at all.
constexpr const char* PlayerRelayEnabled = "player/relay_enabled"; // bool, default false
// How far past its start offset one upstream read may go before it stops,
// in MiB. This is what lets a single request for a far-away region (for
// example an audio track stored at the end of the file) fill the cache for
// every follow-up request on that region.
constexpr const char* PlayerRelayReadaheadMb = "player/relay_readahead_mb"; // int MiB, default 64
// Relay socket write tuning, in KiB. Raising the high-water mark lets mpv take
// more per connection (fewer connections, better throughput) but makes each
// connection more expensive to feed; lowering it cuts that cost at the price of
// more connections. pump_chunk_kb caps a single write per event-loop turn and
// should stay close to what the kernel socket buffer accepts.
constexpr const char* PlayerRelayHighWaterKb = "player/relay_high_water_kb";  // int KiB, default 2048
constexpr const char* PlayerRelayPumpChunkKb = "player/relay_pump_chunk_kb";  // int KiB, default 1024
// Advanced mpv tuning (Settings -> Player). All values are read per file by
// MpvWidget; empty values fall back to mpv defaults.
constexpr const char* PlayerAudioChannels = "player/audio_channels";              // QString: auto|stereo|mono
constexpr const char* PlayerAudioNormalizeDownmix = "player/audio_normalize_downmix"; // bool, default true
constexpr const char* PlayerAudioExclusive = "player/audio_exclusive";            // bool
constexpr const char* PlayerAudioStreamSilence = "player/audio_stream_silence";   // bool
constexpr const char* PlayerStreamBufferSize = "player/stream_buffer_size";       // int KiB, default 128
constexpr const char* PlayerDemuxerMaxBytes = "player/demuxer_max_bytes";         // int MiB, default 1536
constexpr const char* PlayerDemuxerMaxBackBytes = "player/demuxer_max_back_bytes";// int MiB, default 0
constexpr const char* PlayerDemuxerReadaheadSecs = "player/demuxer_readahead_secs";// int s, default 1
constexpr const char* PlayerCurlBackend = "player/curl_backend";                  // bool (prefer libcurl)
constexpr const char* PlayerTcpKeepAlive = "player/tcp_keepalive";                // bool
constexpr const char* PlayerSeekStep = "player/seek_step";
constexpr const char* PlayerLongPressSeek = "player/long_press_seek";
constexpr const char* PlayerLongPressMode = "player/long_press_mode";
constexpr const char* PlayerLongPressTriggerMs = "player/long_press_trigger_ms";
constexpr const char* PlayerMouseEdgeLongPress =
    "player/mouse_edge_long_press";
constexpr const char* PlayerMediaSwitcherMode =
    "player/media_switcher_mode";
constexpr const char* PlayerClickToPause = "player/click_to_pause";
constexpr const char* PlayerSkipIntro = "player/skip_intro";
constexpr const char* PlayerSkipOutro = "player/skip_outro";
constexpr const char* PlayerIndependentWindow = "player/independent_window";
constexpr const char* PlayerUseMpvConf = "player/use_mpv_conf";
constexpr const char* PlayerVolumeLevel = "player/volume_level";
constexpr const char* PlayerVolumeMuted = "player/volume_muted";
constexpr const char* PlayerSubtitleFont = "player/subtitle_font";
constexpr const char* PlayerSubtitleDelayMs = "player/subtitle_delay_ms";
constexpr const char* PlayerSubtitleFontSize = "player/subtitle_font_size";
constexpr const char* PlayerSubtitlePosition = "player/subtitle_position";
constexpr const char* PlayerSubtitleOutlineSize =
    "player/subtitle_outline_size";
constexpr const char* PlayerSubtitleShadowOffset =
    "player/subtitle_shadow_offset";
constexpr const char* PlayerSubtitleScale = "player/subtitle_scale";
// 副字幕（第二条内容字幕）。需先启用 PlayerSubtitleSecondaryEnabled 全局开关；
// 启用后播放器字幕菜单出现副字幕选择、字幕设置对话框出现副字幕参数。副字幕
// 参数未单独设置时回退到主字幕对应值（位置额外偏移，避免两条字幕重叠）。
constexpr const char* PlayerSubtitleSecondaryEnabled =
    "player/subtitle_secondary_enabled";
constexpr const char* PlayerSubtitleSecondaryFont =
    "player/subtitle_secondary_font";
constexpr const char* PlayerSubtitleSecondaryDelayMs =
    "player/subtitle_secondary_delay_ms";
constexpr const char* PlayerSubtitleSecondaryFontSize =
    "player/subtitle_secondary_font_size";
constexpr const char* PlayerSubtitleSecondaryPosition =
    "player/subtitle_secondary_position";
constexpr const char* PlayerSubtitleSecondaryOutlineSize =
    "player/subtitle_secondary_outline_size";
constexpr const char* PlayerSubtitleSecondaryShadowOffset =
    "player/subtitle_secondary_shadow_offset";
constexpr const char* PlayerSubtitleSecondaryScale =
    "player/subtitle_secondary_scale";
// 字幕拖动（拖动开关）：开启后可在播放中按住字幕上下拖动调整位置，松手记忆；
// 关闭时只能通过字幕设置里的位置滑块调整，防止误触。位置范围与滑块一致
// （60-100）。
constexpr const char* PlayerSubtitleDragEnabled =
    "player/subtitle_drag_enabled";
// DV（纯 Dolby Vision）片源自动改用独立窗口播放：内嵌走 render API 时 DV 的
// IPT 色彩空间不被处理（发绿），独立窗口（wid + gpu-next）正常。默认开。
constexpr const char* PlayerDvAutoIndependentWindow =
    "player/dv_auto_independent_window";
constexpr const char* PlayerLastSubtitleDir = "player/last_subtitle_dir";

constexpr const char* PlayerExternalSubtitle = "player/external_subtitle";
constexpr const char* PlayerDanmakuEnabled = "player/danmaku_enabled";
constexpr const char* PlayerDanmakuRenderer = "player/danmaku_renderer";
constexpr const char* PlayerDanmakuOpacity = "player/danmaku_opacity";
constexpr const char* PlayerDanmakuFontScale = "player/danmaku_font_scale";
constexpr const char* PlayerDanmakuFontWeight = "player/danmaku_font_weight";
constexpr const char* PlayerDanmakuOutlineSize =
    "player/danmaku_outline_size";
constexpr const char* PlayerDanmakuShadowOffset =
    "player/danmaku_shadow_offset";
constexpr const char* PlayerDanmakuAreaPercent = "player/danmaku_area_percent";
constexpr const char* PlayerDanmakuDensity = "player/danmaku_density";
constexpr const char* PlayerDanmakuSpeedScale = "player/danmaku_speed_scale";
constexpr const char* PlayerDanmakuOffsetMs = "player/danmaku_offset_ms";
constexpr const char* PlayerDanmakuHideScroll = "player/danmaku_hide_scroll";
constexpr const char* PlayerDanmakuHideTop = "player/danmaku_hide_top";
constexpr const char* PlayerDanmakuHideBottom = "player/danmaku_hide_bottom";
constexpr const char* PlayerDanmakuBlockedKeywords =
    "player/danmaku_blocked_keywords";
constexpr const char* PlayerDanmakuDualSubtitle =
    "player/danmaku_dual_subtitle";




constexpr const char* DanmakuProvider = "danmaku/provider";
constexpr const char* DanmakuProviderBaseUrl = "danmaku/provider_base_url";
constexpr const char* DanmakuProviderAppId = "danmaku/provider_app_id";
constexpr const char* DanmakuProviderAppSecret = "danmaku/provider_app_secret";
constexpr const char* DanmakuServers = "danmaku/servers";
constexpr const char* DanmakuSelectedServer = "danmaku/selected_server";
constexpr const char* DanmakuSourceMode = "danmaku/source_mode";
constexpr const char* DanmakuAutoLoad = "danmaku/auto_load";
constexpr const char* DanmakuAutoMatch = "danmaku/auto_match";
constexpr const char* DanmakuWithRelated = "danmaku/with_related";
constexpr const char* DanmakuCacheHours = "danmaku/cache_hours";
// Which UI the in-player danmaku search opens with:
//   "series"  (default) -> SeriesDanmakuMatchDialog::Single (aggregated
//              series rows + per-episode single picker)
//   "classic"            -> PlayerDanmakuIdentifyDialog (flat
//              episode-level rows, kept for rollback).
constexpr const char* DanmakuPlayerSearchUi = "danmaku/player_search_ui";

// Bilibili danmaku source (QR login required). Account-level, not per-server.
constexpr const char* BilibiliSessData = "danmaku/bilibili/sessdata";
constexpr const char* BilibiliJct = "danmaku/bilibili/jct";
constexpr const char* BilibiliUid = "danmaku/bilibili/uid";
constexpr const char* BilibiliUname = "danmaku/bilibili/uname";
constexpr const char* BilibiliBuvid3 = "danmaku/bilibili/buvid3";




constexpr const char* ExtPlayerEnable = "ext_player/enable";
constexpr const char* ExtPlayerPath = "ext_player/path";
constexpr const char* ExtPlayerArgs = "ext_player/arguments";
constexpr const char* ExtPlayerDirectStream = "ext_player/direct_stream";
constexpr const char* ExtPlayerQuickPlay = "ext_player/quick_play";
constexpr const char* ExtPlayerUrlReplace = "ext_player/url_replace";
constexpr const char* ExtPlayerDetectedList = "ext_player/detected_list";




// Trakt.tv integration (built-in shared credentials, device code login)
constexpr const char* TraktAccessToken = "trakt/access_token";
constexpr const char* TraktRefreshToken = "trakt/refresh_token";
constexpr const char* TraktUserSlug = "trakt/user_slug";
constexpr const char* TraktUserName = "trakt/user_name";
constexpr const char* TraktScrobbleEnabled = "trakt/scrobble_enabled";
constexpr const char* TraktSyncButtonEnabled = "trakt/sync_button_enabled";
constexpr const char* TraktResumeCheckEnabled = "trakt/resume_check_enabled";




constexpr const char* SearchHistoryRecords = "search/history_records"; 




constexpr const char* DownloadDirectory = "download/directory";
constexpr const char* DownloadHistoryRecords = "download/history_records";
constexpr const char* DownloadDeleteFileWithRecord =
    "download/delete_file_with_record";
}

#endif 
