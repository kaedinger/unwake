<?php
/* Unwake - shared helpers
 * Required by both Unwake.php (page render) and exec.php (POST handler).
 */

const UNWAKE_CFG      = '/boot/config/plugins/unwake/unwake.cfg';
const UNWAKE_DEFAULTS = '/usr/local/emhttp/plugins/unwake/unwake.cfg.default';
const UNWAKE_LOG      = '/var/log/unwake.log';
const UNWAKE_RUN      = '/var/run/unwake';
const UNWAKE_RC       = '/etc/rc.d/rc.unwake';
const UNWAKED         = '/usr/local/sbin/unwaked';
const UNWAKE_PLG      = '/boot/config/plugins/unwake.plg';
const UNWAKE_MNT      = '/mnt/user';
const DISKS_INI       = '/var/local/emhttp/disks.ini';
const SHARES_DIR      = '/boot/config/shares';

// release channels: the stable .plg is a release asset; releases are immutable, so the
// rolling beta .plg lives on the `channel` branch
const UNWAKE_REPO     = 'https://github.com/kaedinger/unwake';
const CHANNEL_URLS    = [
    'stable' => UNWAKE_REPO . '/releases/latest/download/unwake.plg',
    'beta'   => 'https://raw.githubusercontent.com/kaedinger/unwake/channel/unwake.plg',
];

function load_cfg($path) {
    $cfg = [];
    if (!file_exists($path)) return $cfg;
    foreach (file($path, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
        $line = trim($line);
        if ($line === '' || $line[0] === '#') continue;
        if (strpos($line, '=') !== false) {
            [$k, $v] = explode('=', $line, 2);
            $cfg[trim($k)] = trim($v, " \t\"'");
        }
    }
    return $cfg;
}

// What unwaked makes of a settings file (unwaked --page-info): ok, warnings, settings
// (each as in use: the saved ones over the defaults, invalid ones replaced, the trim size
// fitted), pool_only (shares it passes to shfs by their settings), limits (numbers the help
// texts name). The rules live in unwaked only. null if it can't be asked.
function page_info($cfg_file = UNWAKE_CFG) {
    static $cache = [];   // per request, by the file's content (a save changes it)
    $key = $cfg_file . ':' . @md5_file($cfg_file);
    if (!isset($cache[$key])) {
        $out = [];
        exec(UNWAKED . ' --page-info --config ' . escapeshellarg($cfg_file) . ' 2>/dev/null', $out);
        $cache[$key] = json_decode(implode("\n", $out), true);
    }
    return is_array($cache[$key]) ? $cache[$key] : null;
}

// The settings in effect. Without unwaked's answer: the saved ones over the defaults, unchecked
function settings() {
    $pi = page_info();
    return !empty($pi['ok']) ? $pi['settings'] : [...load_cfg(UNWAKE_DEFAULTS), ...load_cfg(UNWAKE_CFG)];
}

// numbers the help texts name (from unwaked), 0 when not known
function limit($name) { return page_info()['limits'][$name] ?? 0; }

// The settings chosen from a list: the label of each value, in the page's order
// (which values are valid is unwaked's to say; see page_info)
const CHOICES = [
    'SERVICE'    => ['disabled' => 'No', 'enabled' => 'Yes'],
    'HARVEST'    => ['yes' => 'Yes', 'no' => 'No - only what is browsed'],
    'DATA_PATH'  => ['direct' => 'From their disk, when known', 'shfs' => 'Through shfs'],
    'RETURN_RAM' => ['no' => 'No', 'yes' => 'Yes'],
    'LOG_LEVEL'  => ['info' => 'Info', 'debug' => 'Debug (verbose - every change and cache miss)'],
];
// The settings typed in as numbers
const NUMBERS = ['LOG_MAX_SIZE_MB', 'LOG_TRIM_SIZE_MB'];

function is_debug($cfg) { return ($cfg['LOG_LEVEL'] ?? '') === 'debug'; }

// a comma-separated setting (EXCLUDED_SHARES, INCLUDED_SHARES) as a list
function cfg_list($cfg, $key) { return array_values(array_filter(array_map('trim', explode(',', $cfg[$key] ?? '')))); }

function pid_alive($pid) { return $pid > 0 && file_exists("/proc/$pid"); }

// The daemon's own report, or null when it isn't running
function daemon_status() {
    $pid = (int)@file_get_contents(UNWAKE_RUN . '/unwaked.pid');
    $st  = @json_decode(@file_get_contents(UNWAKE_RUN . '/status.json'), true);
    $older = [];   // detached instances still serving something: when each was stopped
    foreach (glob(UNWAKE_RUN . '/draining.*') ?: [] as $f)
        if (pid_alive((int)@file_get_contents($f))) $older[] = (int)@filemtime($f);
    sort($older);
    $draining = count($older);
    if (!pid_alive($pid)) return ['running' => false, 'draining' => $draining, 'older' => $older];
    $st = is_array($st) ? $st : [];
    $st['running']  = true;
    $st['draining'] = $draining;
    $st['older']    = $older;
    return $st;
}

// What is mounted on /mnt/user right now (the topmost mount)
function user_mount_type() {
    $t = '';
    foreach (@file('/proc/self/mountinfo') ?: [] as $line) {
        $f = explode(' ', $line);
        if (($f[4] ?? '') !== UNWAKE_MNT) continue;
        $sep = array_search('-', $f);
        if ($sep !== false) $t = $f[$sep + 1] ?? '';
    }
    return $t;
}

// Dynamix Cache Directories keeps scanning: Unwake makes it unnecessary
// Dynamix Cache Directories' daemon. Its plugin also runs cache_dirs briefly (status,
// stop, plugin checks after an install): only a process older than 15 s counts.
function cache_dirs_running() {
    exec("pgrep -f '[c]ache_dirs' 2>/dev/null", $pids);   // [c]: not this shell itself
    foreach ($pids as $pid) {
        $age = (int)trim((string)@shell_exec('ps -o etimes= -p ' . (int)$pid . ' 2>/dev/null'));
        if ($age > 15) return true;
    }
    return false;
}

// vm.vfs_cache_pressure, or null if it can't be read
const VFS_PRESSURE = '/proc/sys/vm/vfs_cache_pressure';
function vfs_pressure() {
    $v = @file_get_contents(VFS_PRESSURE);
    return ($v === false || !is_numeric(trim($v))) ? null : (int)trim($v);
}

// version, pluginURL and CHANGES of a .plg file, or null
function plg_info($file) {
    if (!is_file($file)) return null;
    $xml = @simplexml_load_file($file, 'SimpleXMLElement', LIBXML_NOENT);
    if (!$xml || !isset($xml['version'])) return null;
    return [
        'version' => (string)$xml['version'],
        'url'     => (string)$xml['pluginURL'],
        'changes' => trim((string)$xml->CHANGES),
    ];
}

// the installed version and the channel it updates from (the .plg on the flash)
function installed() {
    $info = plg_info(UNWAKE_PLG);
    if (!$info) return ['version' => 'unknown', 'channel' => 'stable'];
    return ['version' => $info['version'], 'channel' => $info['url'] === CHANNEL_URLS['beta'] ? 'beta' : 'stable'];
}

// user shares from Unraid's share configs: name => [comment, pool_only]
// pool_only: its settings keep it on an SSD pool only - it can't wake a disk, so Unwake
// leaves it to shfs unless told otherwise (unwaked decides: page_info)
function load_unraid_shares() {
    $out = [];
    $pool_only = page_info()['pool_only'] ?? [];
    $mounted = user_mount_type() !== '';
    foreach (glob(SHARES_DIR . '/*.cfg') ?: [] as $f) {
        $name = basename($f, '.cfg');
        if ($mounted && !is_dir(UNWAKE_MNT . "/$name")) continue;   // deleted share
        $c = load_cfg($f);
        $out[$name] = ['comment' => $c['shareComment'] ?? '', 'pool_only' => in_array($name, $pool_only, true)];
    }
    ksort($out, SORT_NATURAL | SORT_FLAG_CASE);
    return $out;
}

// What Unwake decided for an SSD-pool-only share (running daemon's report): text for the page
function pool_only_text($name, $st) {
    $p = $st['pool_only'][$name] ?? null;
    if (!$st['running'] || $p === null) return 'SSD pool only - checked against the array disks while Unwake runs';
    if (!empty($p['on'])) return 'SSD pool only, but ' . implode(', ', $p['on']) . (count($p['on']) == 1 ? ' holds' : ' hold') . ' folders of it - kept in RAM';
    if (empty($p['known'])) return 'SSD pool only - passed to shfs (array disks not all read yet)';
    return 'SSD pool only, can\'t wake a disk - passed to shfs';
}

// is a share cached, by the settings and the pool-only rule
function share_cached($name, $info, $cfg) {
    if (in_array($name, cfg_list($cfg, 'EXCLUDED_SHARES'), true)) return false;
    if (in_array($name, cfg_list($cfg, 'INCLUDED_SHARES'), true)) return true;
    return !$info['pool_only'];
}

function fmt_bytes($n) {
    $u = ['B','KB','MB','GB','TB'];
    $i = 0;
    $v = (float)$n;
    while ($v >= 1024 && $i < count($u) - 1) { $v /= 1024; $i++; }
    return ($v >= 100 ? round($v) : round($v, 1)) . ' ' . $u[$i];
}

function fmt_count($n) {
    if ($n >= 1e6) return round($n / 1e6, $n >= 1e7 ? 0 : 1) . 'M';
    if ($n >= 1e4) return round($n / 1e3) . 'k';
    return number_format($n);
}

function fmt_dur($ms) {
    $s = (int)round($ms / 1000);
    if ($s >= 3600) return intdiv($s, 3600) . 'h' . ($s % 3600 >= 60 ? ' ' . intdiv($s % 3600, 60) . 'm' : '');
    if ($s >= 60) return intdiv($s, 60) . 'm' . ($s % 60 ? ' ' . ($s % 60) . 's' : '');
    return $s . 's';
}

function fmt_when($epoch) {
    return date(date('Y-m-d', $epoch) === date('Y-m-d') ? 'H:i' : 'M j H:i', $epoch);
}

// Dot before a disk name: green while spinning, an empty circle while spun down
function spin_mark($spinning, $ssd = false) {
    if ($ssd) return '<span class="uw-spin on" title="No spin-down (SSD)"></span>';
    return $spinning ? '<span class="uw-spin on" title="Spinning"></span>' : '<span class="uw-spin" title="Spun down"></span>';
}

// Detached instances that still serve something (stopped or restarted while in use)
function older_text($older) {
    if (!$older) return '';
    $when = implode(', ', array_map(fn($t) => $t ? fmt_when($t) : '?', $older));
    return count($older) === 1
        ? 'An older Unwake is still in use (stopped ' . $when . '). It exits once nothing uses it any more.'
        : count($older) . ' older Unwake instances are still in use (stopped ' . $when . '). Each one exits once nothing uses it any more.';
}

// The button next to it, in the same number
function older_button($older) {
    return count($older) === 1 ? 'Who uses it?' : 'Who uses them?';
}

// Status sections for the page and the poll: summary lines and detail tables (HTML).
// Colors: the page's CSS variables (Unwake.php)
function status_view($st) {
    $v = ['status' => '', 'color' => 'var(--uw-bad)', 'cache' => '', 'cache_detail' => '', 'hits' => '', 'hits_detail' => '',
          'disks' => '', 'verify' => '', 'verify_detail' => '', 'pool_only' => [],
          'toggle' => $st['running'] ? 'Stop Unwake' : 'Start Unwake',
          'version' => $st['version'] ?? '', 'older' => older_text($st['older'] ?? []), 'older_btn' => older_button($st['older'] ?? [])];
    foreach (load_unraid_shares() as $name => $info)
        if ($info['pool_only']) $v['pool_only'][$name] = pool_only_text($name, $st);
    $mt = user_mount_type();
    if (!$st['running']) {
        $v['status'] = $mt === '' ? 'Stopped (array not started)' : 'Stopped';
        return $v;
    }
    $state = $st['state'] ?? 'starting';
    if ($state === 'running' && $mt === 'fuse.unwake') { $v['status'] = 'Running'; $v['color'] = 'var(--uw-ok)'; }
    elseif ($state === 'draining') { $v['status'] = 'Detached - serving open files until they are closed'; $v['color'] = 'var(--uw-warn)'; }
    elseif ($state === 'running') { $v['status'] = 'Running, but not on top of /mnt/user'; $v['color'] = 'var(--uw-warn)'; }
    else { $v['status'] = ucfirst($state); $v['color'] = 'var(--uw-warn)'; }
    if (empty($st['caching']) && $state === 'running') { $v['status'] .= ' (pass-through only, caching off - see the log)'; $v['color'] = 'var(--uw-warn)'; }

    $v['cache'] = fmt_count($st['dirs'] ?? 0) . ' folders, ' . fmt_count($st['entries'] ?? 0) . ' names, ' .
                  fmt_bytes(($st['rss_kb'] ?? 0) * 1024) . ' RAM';
    $v['cache_detail'] = '<div class="uw-grid uw-grid-2">' .
        '<span>Folders</span><span>' . number_format($st['dirs'] ?? 0) . '</span>' .
        '<span>Names</span><span>' . number_format($st['entries'] ?? 0) . '</span>' .
        '<span>Names the kernel holds an inode for</span><span>' . number_format($st['inodes'] ?? 0) . '</span>' .
        '<span>RAM used by unwaked</span><span>' . fmt_bytes(($st['rss_kb'] ?? 0) * 1024) . '</span>' .
        (isset($st['kept_free_kb']) && settings()['RETURN_RAM'] !== 'yes' ? '<span>of it freed, kept for reuse</span><span>' . fmt_bytes($st['kept_free_kb'] * 1024) . '</span>' : '') .
        '<span>Changes seen on the disks</span><span>' . number_format($st['events'] ?? 0) . '</span>' .
        '<span>File data</span><span>' . (!empty($st['passthrough']) ? 'kernel passthrough' : 'through unwaked') . '</span>' .
        '<span>Running since</span><span>' . (isset($st['started']) ? fmt_when($st['started']) : '-') . '</span></div>';

    $rows = [['Listings', 'readdir'], ['Name lookups', 'lookup'], ['Attributes', 'getattr'], ['Extended attributes', 'xattr']];
    $hit = $all = 0;
    $tbl = '<div class="uw-grid uw-grid-4"><span></span><span>From RAM</span><span>From shfs</span><span>Share</span>';
    foreach ($rows as [$label, $k]) {
        $h = ($st["{$k}_hit"] ?? 0) + ($k === 'lookup' ? ($st['lookup_neg'] ?? 0) : 0);
        $m = $st["{$k}_miss"] ?? 0;
        $hit += $h; $all += $h + $m;
        $tbl .= "<span>$label</span><span>" . number_format($h) . '</span><span>' . number_format($m) . '</span><span>' .
                ($h + $m ? round(100 * $h / ($h + $m), 1) . '%' : '-') . '</span>';
    }
    $tbl .= '<span>Name lookups of missing files</span><span>' . number_format($st['lookup_neg'] ?? 0) . '</span><span></span><span></span>';
    $tbl .= '<span>Files read straight from their disk</span><span>' . number_format($st['open_direct'] ?? 0) . '</span><span>' .
            number_format($st['open_shfs'] ?? 0) . '</span><span></span></div>';
    $v['hits'] = $all ? round(100 * $hit / $all, 1) . '% of ' . fmt_count($all) . ' requests answered from RAM' : 'No requests yet';
    $v['hits_detail'] = $tbl;

    // stables verify too, but only betas show it (Verify section, "verified" in Disks and Pools)
    $beta = substr($st['version'] ?? '', -5) === '-beta';
    $cards = '';
    foreach ($st['branches'] ?? [] as $b) {
        $ssd = empty($b['rotational']);
        if (!empty($b['untracked'])) $state = '<span class="red-text">changes can\'t be watched</span>';
        elseif (!empty($b['harvesting'])) $state = 'reading its folders now';
        elseif ($beta && !empty($b['harvested']) && ($b['verify']['done_at'] ?? 0) > ($b['harvested_at'] ?? 0))
            $state = fmt_count($b['dirs']) . ' folders, verified ' . fmt_when($b['verify']['done_at']);
        elseif (!empty($b['harvested'])) $state = fmt_count($b['dirs']) . ' folders, read ' . fmt_when($b['harvested_at']) .
                                                  ' in ' . fmt_dur($b['harvest_ms']);
        else $state = $b['up'] ? 'waiting' : 'read when it next spins up';
        $cards .= '<div class="uw-disk-row"><span class="uw-disk-name">' . spin_mark(!empty($b['up']), $ssd) . ' ' .
                  htmlspecialchars($b['name']) . '</span><span class="uw-muted">' . $state . '</span></div>';
    }
    $v['disks'] = $cards;

    // the verifier (always on in betas, shown in betas only): its own section, one line per disk, the current pass only
    if ($beta && !empty($st['verifying'])) {
        $diffs = function ($f, $r, $p) {
            if (!$f) return '0 differences';
            $n = number_format($f) . ' difference' . ($f == 1 ? '' : 's');
            if ($p) return "$n, repairing";
            return $r >= $f ? "$n repaired" : "$n, " . number_format($r) . ' repaired';
        };
        $busy = false; $f = $r = $p = 0;
        foreach ($st['branches'] ?? [] as $b) {
            $vf = $b['verify'] ?? [];
            $state = $vf['state'] ?? 'waiting';
            $name = htmlspecialchars($b['name']);
            $c = (int)($vf['checked'] ?? 0); $t = (int)($vf['total'] ?? 0);
            $bf = (int)($vf['found'] ?? 0); $br = (int)($vf['repaired'] ?? 0); $bp = (int)($vf['pending'] ?? 0);
            $f += $bf; $r += $br; $p += $bp;
            $d = $diffs($bf, $br, $bp);
            if ($bf && $br < $bf) $d = '<span class="red-text">' . $d . '</span>';
            if ($state === 'done') {
                $line = "Verified $name: " . fmt_when($vf['done_at'] ?? 0) . ', ' . number_format($c) . ' of ' . fmt_count($t) . " folders checked, $d";
            } elseif ($state === 'running') {
                $busy = true;
                $line = "Verifying $name: " . number_format($c) . ' of ' . fmt_count($t) . " folders checked, $d";
            } else {
                $busy = true;
                $line = "Verifying $name: waiting" . ($c ? ', ' . number_format($c) . ' of ' . fmt_count($t) . " folders checked, $d" : '...');
            }
            $v['verify_detail'] .= '<div class="uw-col-text"' . ($state === 'done' ? ' style="color:var(--uw-ok);"' : '') . '>' . $line . '</div>';
        }
        $v['verify'] = ($busy ? 'Verifying, ' : 'Verify complete, ') . $diffs($f, $r, $p);
    }
    return $v;
}

// The log box: the newest entries, more of them in debug mode
function log_text($cfg) { return log_tail(is_debug($cfg) ? 1000 : 200) ?: 'No log entries yet.'; }

// The newest $entries log entries, exact repeats in a row folded into one:
// "<first time> [DEBUG] readdir hit x (25x until <last time>)". An entry is a line with
// a timestamp plus the indented lines below it.
function log_tail($entries) {
    $out = [];
    exec('tail -n ' . (int)($entries * 20) . ' ' . escapeshellarg(UNWAKE_LOG) . ' 2>/dev/null', $out);
    $groups = [];   // [first ts, last ts, body (first line after ts + continuation lines), count]
    $cur = null;
    $flush = function () use (&$cur, &$groups) {
        if ($cur === null) return;
        $n = count($groups);
        if ($n && $groups[$n - 1][2] === $cur[1]) { $groups[$n - 1][1] = $cur[0]; $groups[$n - 1][3]++; }
        else $groups[] = [$cur[0], $cur[0], $cur[1], 1];
        $cur = null;
    };
    foreach ($out as $line) {
        if (preg_match('/^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)$/', $line, $m)) { $flush(); $cur = [$m[1], $m[2]]; }
        elseif ($cur !== null) $cur[1] .= "\n" . $line;
        // continuation lines cut off at the start of the tail are dropped
    }
    $flush();
    $lines = [];
    foreach (array_slice($groups, -$entries) as [$first, $last, $body, $n]) {
        if ($n > 1) {
            $until = substr($first, 0, 10) === substr($last, 0, 10) ? substr($last, 11) : $last;
            $nl = strpos($body, "\n");
            $suffix = " ({$n}x until $until)";
            $body = $nl === false ? $body . $suffix : substr($body, 0, $nl) . $suffix . substr($body, $nl);
        }
        $lines[] = "$first $body";
    }
    return implode("\n", $lines);
}
