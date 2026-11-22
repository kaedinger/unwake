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

function rc($arg) {
    exec(UNWAKE_RC . ' ' . $arg . ' 2>&1', $out, $rc);
    return [$rc === 0, trim(implode(' ', $out))];
}

function poll_result($cfg) {
    $st = daemon_status();
    return ['running' => $st['running'], 'view' => status_view($st), 'cache_dirs' => cache_dirs_running(), 'pressure' => vfs_pressure(),
            'log' => log_tail(is_debug($cfg) ? 1000 : 200)];
}

$action = $_POST['action'] ?? '';
$cfg    = settings();
$ok     = true;
$msg    = '';

switch ($action) {
case 'save':
    $defs = load_cfg(UNWAKE_DEFAULTS);
    $prev = load_cfg(UNWAKE_CFG);
    $p    = fn($k)        => $prev[$k] ?? $defs[$k] ?? '';
    $choice = fn($k) => array_key_exists($_POST[$k] ?? '', CHOICES[$k]) ? $_POST[$k] : $p($k);
    $pint = fn($k, $m = 1) => isset($_POST[$k]) && ($v = intval($_POST[$k])) >= $m ? $v : intval($p($k));

    $new = [];
    foreach (array_keys(CHOICES) as $k) $new[$k] = $choice($k);
    // SSD-pool-only shares are decided by unwaked (no checkbox); entries made for them
    // by hand in unwake.cfg are kept. Other shares: unticked = excluded.
    $list = fn($k) => array_filter(array_map('trim', explode(',', $cfg[$k] ?? '')));
    $excluded = $included = [];
    foreach (load_unraid_shares() as $name => $info) {
        if ($info['pool_only']) {
            if (in_array($name, $list('EXCLUDED_SHARES'), true)) $excluded[] = $name;
            if (in_array($name, $list('INCLUDED_SHARES'), true)) $included[] = $name;
            continue;
        }
        if (($_POST['SHARE_CACHE_' . $name] ?? '1') === '0') $excluded[] = $name;
    }
    sort($excluded); sort($included);
    $new['EXCLUDED_SHARES']  = implode(',', $excluded);
    $new['INCLUDED_SHARES']  = implode(',', $included);
    $new['LOG_MAX_SIZE_MB']  = $pint('LOG_MAX_SIZE_MB');
    $new['LOG_TRIM_SIZE_MB'] = $pint('LOG_TRIM_SIZE_MB');
    if ($new['LOG_TRIM_SIZE_MB'] >= $new['LOG_MAX_SIZE_MB']) $new['LOG_TRIM_SIZE_MB'] = max(1, intdiv($new['LOG_MAX_SIZE_MB'], 2));
    $new += $prev;   // keys the page doesn't show (VERIFY_*, ...) are kept as they are
    save_cfg(UNWAKE_CFG, $new);

    $st = daemon_status();
    if ($new['SERVICE'] === 'enabled' && !$st['running']) {
        if (user_mount_type() === '') $msg = 'Settings saved. Unwake starts with the array.';
        else { [$ok, $out] = rc('start'); $msg = $ok ? 'Settings saved, Unwake started.' : "Settings saved, but: $out"; }
    } elseif ($new['SERVICE'] === 'disabled' && $st['running']) {
        [$ok, $out] = rc('stop');
        $msg = "Settings saved. $out";
    } elseif ($st['running']) {
        rc('reload');
        $msg = 'Settings saved and applied.';
    } else $msg = 'Settings saved.';
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
    exec(UNWAKE_RC . ' users 2>&1', $out);
    echo json_encode(['ok' => true, 'text' => implode("\n", $out)]);
    break;

case 'clear_log':
    @file_put_contents(UNWAKE_LOG, '');
    echo json_encode(['ok' => true, 'message' => 'Log cleared.']);
    break;

default:
    http_response_code(400);
    echo json_encode(['ok' => false, 'message' => 'unknown action']);
}
