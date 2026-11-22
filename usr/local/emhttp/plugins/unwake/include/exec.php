<?php
/* Unwake - POST action handler
 * Served at /plugins/unwake/include/exec.php
 * Called via jQuery $.post() from Unwake.php.
 */

header('Content-Type: application/json');
require_once __DIR__ . '/cfg.php';

function save_cfg($path, $cfg) {
    $body = "# Unwake Configuration - managed by UI\n# " . date('Y-m-d H:i:s') . "\n\n";
    foreach ($cfg as $k => $v) $body .= "$k=\"$v\"\n";
    @mkdir(dirname($path), 0755, true);
    file_put_contents($path, $body);
}

// rc.unwake: [succeeded, its output]
function rc($arg, $glue = ' ') {
    exec(UNWAKE_RC . ' ' . $arg . ' 2>&1', $out, $rc);
    return [$rc === 0, trim(implode($glue, $out))];
}

function poll_result($cfg) {
    $st = daemon_status();
    return ['running' => $st['running'], 'view' => status_view($st), 'cache_dirs' => cache_dirs_running(), 'pressure' => vfs_pressure(),
            'log' => log_text($cfg)];
}

$action = $_POST['action'] ?? '';
$cfg    = settings();
$ok     = true;
$msg    = '';

switch ($action) {
case 'save':
    // what the page sends, over what was saved (keys it doesn't show, VERIFY_*, stay);
    // unwaked checks it and says what each setting becomes
    $prev = load_cfg(UNWAKE_CFG);
    $new = $prev;
    foreach ([...array_keys(CHOICES), ...NUMBERS] as $k)
        if (isset($_POST[$k])) $new[$k] = trim((string)$_POST[$k]);
    // SSD-pool-only shares are decided by unwaked (no checkbox); entries made for them
    // by hand in unwake.cfg are kept. Other shares: unticked = excluded.
    $excluded = $included = [];
    foreach (load_unraid_shares() as $name => $info) {
        if ($info['pool_only']) {
            if (in_array($name, cfg_list($cfg, 'EXCLUDED_SHARES'), true)) $excluded[] = $name;
            if (in_array($name, cfg_list($cfg, 'INCLUDED_SHARES'), true)) $included[] = $name;
            continue;
        }
        if (($_POST['SHARE_CACHE_' . $name] ?? '1') === '0') $excluded[] = $name;
    }
    sort($excluded); sort($included);
    $new['EXCLUDED_SHARES'] = implode(',', $excluded);
    $new['INCLUDED_SHARES'] = implode(',', $included);
    $tmp = tempnam(sys_get_temp_dir(), 'unwake-cfg');
    save_cfg($tmp, $new);
    $pi = page_info($tmp);
    @unlink($tmp);
    if (empty($pi['ok'])) {
        $why = $pi ? implode('; ', $pi['warnings']) : 'unwaked could not check them';
        echo json_encode(['ok' => false, 'message' => "Settings not saved: $why", 'cfg' => $cfg] + poll_result($cfg));
        break;
    }
    foreach ($new as $k => $v) if (isset($pi['settings'][$k])) $new[$k] = $pi['settings'][$k];
    save_cfg(UNWAKE_CFG, $new);
    $warn = $pi['warnings'] ? ' (' . implode('; ', $pi['warnings']) . ')' : '';

    $st = daemon_status();
    if ($pi['settings']['SERVICE'] === 'enabled' && !$st['running']) {
        if (user_mount_type() === '') $msg = 'Settings saved. Unwake starts with the array.';
        else { [$ok, $out] = rc('start'); $msg = $ok ? 'Settings saved, Unwake started.' : "Settings saved, but: $out"; }
    } elseif ($pi['settings']['SERVICE'] === 'disabled' && $st['running']) {
        [$ok, $out] = rc('stop');
        $msg = "Settings saved. $out";
    } elseif ($st['running']) {
        rc('reload');
        $msg = 'Settings saved and applied.';
    } else $msg = 'Settings saved.';
    $msg .= $warn;
    $cfg = settings();
    echo json_encode(['ok' => $ok, 'message' => $msg, 'cfg' => $cfg] + poll_result($cfg));
    break;

case 'start':
case 'stop':
case 'restart':
    [$ok, $out] = rc($action);
    echo json_encode(['ok' => $ok, 'message' => $out] + poll_result($cfg));
    break;

case 'poll':
    echo json_encode(['ok' => true] + poll_result($cfg));
    break;

case 'set_pressure':
    // the kernel default; lost at reboot, which brings back 100 anyway
    $ok  = @file_put_contents(VFS_PRESSURE, "100\n") !== false && vfs_pressure() === 100;
    $msg = $ok ? 'vfs_cache_pressure set to 100.' : 'Could not set vfs_cache_pressure.';
    if ($ok) exec('logger -t unwake ' . escapeshellarg('vfs_cache_pressure set to 100 from the settings page'));
    echo json_encode(['ok' => $ok, 'message' => $msg] + poll_result($cfg));
    break;

case 'users':
    echo json_encode(['ok' => true, 'text' => rc('users', "\n")[1]]);
    break;

case 'clear_log':
    @file_put_contents(UNWAKE_LOG, '');
    echo json_encode(['ok' => true, 'message' => 'Log cleared.']);
    break;

default:
    http_response_code(400);
    echo json_encode(['ok' => false, 'message' => 'unknown action']);
}
