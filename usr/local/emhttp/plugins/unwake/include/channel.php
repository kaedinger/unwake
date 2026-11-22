<?php
/* unwake - release channels (stable / beta)
 * Served at /plugins/unwake/include/channel.php
 * Which channel is installed, and the newest version on each channel.
 */

require_once __DIR__ . '/cfg.php';

const UNWAKE_CH_CACHE  = '/tmp/unwake-channel';
const UNWAKE_CH_TTL    = 3600;
const PLUGINS_TMP      = '/tmp/plugins';   // Unraid's: its update check's copy, release notes for its popup

// newest .plg of a channel, cached for UNWAKE_CH_TTL; a stale copy is used when GitHub is unreachable.
// Also refreshed once Unraid's own update check (Plugins page) ran after the cache was filled,
// so this page never shows less than the Plugins page.
function channel_latest($ch, $refresh) {
    @mkdir(UNWAKE_CH_CACHE, 0755, true);
    $file  = UNWAKE_CH_CACHE . "/$ch.plg";
    $fresh = is_file($file) && time() - filemtime($file) < UNWAKE_CH_TTL &&
             @filemtime(PLUGINS_TMP . '/unwake.plg') <= filemtime($file);
    if ($refresh || !$fresh) {
        $tmp = "$file.tmp";
        $rc  = 1;
        exec('curl -fsSL --max-time 10 -o ' . escapeshellarg($tmp) . ' ' . escapeshellarg(CHANNEL_URLS[$ch]) . ' 2>/dev/null', $out, $rc);
        if ($rc === 0 && plg_info($tmp)) rename($tmp, $file); else @unlink($tmp);
    }
    $info = plg_info($file);
    if (!$info) return null;
    // release notes for Unraid's own popup (showchanges only reads .txt files under /tmp/plugins)
    @mkdir(PLUGINS_TMP, 0755, true);
    $txt = PLUGINS_TMP . "/unwake-$ch.txt";
    file_put_contents($txt, $info['changes']);
    return ['version' => $info['version'], 'url' => CHANNEL_URLS[$ch], 'notes' => $txt];
}

header('Content-Type: application/json');
$action = $_REQUEST['action'] ?? '';
if ($action === 'versions') {
    $refresh = !empty($_GET['refresh']);
    echo json_encode([
        'installed' => installed(),
        'stable'    => channel_latest('stable', $refresh),
        'beta'      => channel_latest('beta', $refresh),
    ]);
} elseif ($action === 'check' && $_SERVER['REQUEST_METHOD'] === 'POST') {
    // after a switch or update: Unraid's update check follows the new pluginURL right away
    exec('/usr/local/sbin/plugin check unwake.plg >/dev/null 2>&1');
    array_map('unlink', glob(UNWAKE_CH_CACHE . '/*.plg') ?: []);
    echo json_encode(['ok' => true]);
} else {
    http_response_code(400);
    echo json_encode(['error' => 'unknown action']);
}
