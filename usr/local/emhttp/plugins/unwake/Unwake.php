<?php
/* Unwake - Settings page for Unraid
 * Included by Unwake.page. All POST actions go to include/exec.php via $.post().
 * Look and feel follow the Unspin plugin.
 */

require_once __DIR__ . '/include/cfg.php';

function opt($val, $current, $label) {
    $sel = ($current === $val) ? ' selected' : '';
    return '<option value="' . htmlspecialchars($val) . "\"$sel>" . htmlspecialchars($label) . '</option>';
}

$defaults  = load_cfg(UNWAKE_DEFAULTS);
$c         = settings();
$version   = installed_version();
$plg       = @file_get_contents('/boot/config/plugins/unwake.plg') ?: '';
$channel   = strpos($plg, 'raw.githubusercontent.com/kaedinger/unwake/channel/unwake.plg') !== false ? 'beta' : 'stable';
$st        = daemon_status();
$view      = status_view($st);
$cache_dirs = cache_dirs_running();
$pressure  = vfs_pressure();
$shares    = load_unraid_shares();

$log_lines = log_tail(is_debug($c) ? 1000 : 200);
?>

<style>
.uw-spinner {
  display: inline-block; width: 11px; height: 11px;
  border: 2px solid currentColor; border-top-color: transparent; border-radius: 50%;
  opacity: 0.7; animation: uw-spin 0.7s linear infinite; vertical-align: middle; margin-right: 5px;
}
@keyframes uw-spin { to { transform: rotate(360deg); } }

/* Width of the settings column, as Unraid's form elements */
:root { --uw-col: 400px; }

.uw-muted { color: #888; }
dt.uw-has-help { cursor: help; user-select: none; }

/* Help text panels - hidden by default */
.uw-help {
  overflow: hidden; max-height: 0; max-width: var(--uw-col); opacity: 0;
  transition: max-height 0.25s ease, opacity 0.2s ease, padding 0.2s ease, margin-top 0.2s ease, border-color 0.2s ease;
  background: #fff; color: #111; font-size: 0.9em; line-height: 1.5;
  border-radius: 4px; border: 1px solid transparent; padding: 0 10px; margin-top: 0; box-sizing: border-box;
}
.uw-help.open { max-height: 400px; opacity: 1; padding: 7px 10px; margin-top: 5px; border-color: #ccc; }

#uw-toggle-btn, #uw-apply-btn { width: var(--uw-col) !important; }

.uw-row { display: flex; align-items: center; gap: 10px; }

/* Foldable summary line */
.uw-fold-line {
  width: var(--uw-col); max-width: 100%; cursor: pointer; user-select: none;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.uw-grid {
  display: grid; justify-content: space-between; width: var(--uw-col); max-width: 100%;
  column-gap: 16px; row-gap: 2px; margin: 8px 0 4px;
}
.uw-grid > span { white-space: nowrap; }
.uw-grid-2 { grid-template-columns: auto auto; }
.uw-grid-4 { grid-template-columns: auto auto auto auto; }
.uw-grid-4 > span:nth-child(-n+4) { opacity: 0.7; }

/* Disks and pools */
.uw-disk-row { display: flex; align-items: baseline; gap: 8px; width: var(--uw-col); max-width: 100%; }
.uw-disk-name { font-family: monospace; flex: 0 0 7.2em; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.uw-disk-row .uw-muted { flex: 1 1 auto; min-width: 0; }
.uw-spin {
  display: inline-block; width: 8px; height: 8px; box-sizing: border-box;
  border: 1px solid #888; border-radius: 50%; vertical-align: middle; position: relative; top: -1px;
}
.uw-spin.on { background: #4ade80; border-color: #4ade80; }

/* Shares */
dt.uw-share-name { text-align: right; font-weight: normal; }
.uw-share-row { display: grid; grid-template-columns: 7.5em 1fr; align-items: center; gap: 10px; width: var(--uw-col); max-width: 100%; }
.uw-share-row label { white-space: nowrap; }

.uw-warn { color: #facc15; }
.uw-col-text { width: var(--uw-col); max-width: 100%; }
.uw-bordered { border: 1px solid rgba(128,128,128,0.35); border-radius: 4px; }

/* Author footer */
.uw-footer { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; margin-top: 12px; font-size: 0.85em; }
.uw-footer-avatar { width: 28px; height: 28px; border-radius: 50%; }
.uw-footer-sep { opacity: 0.35; }
/* Release channel switch, bottom right */
#uw-channel { margin-left: auto; display: flex; align-items: center; gap: .6em; flex-wrap: wrap; }
#uw-channel .uw-ch-v { opacity: .6; }
#uw-channel .uw-ch-v.red-text { opacity: 1; }
#uw-channel .fa-info-circle { cursor: pointer; }
#uw-channel .switch-button-label { white-space: nowrap; font-size: inherit; }
#uw-channel #uw-ch-go { margin: 0; padding: 0 1em; height: auto; min-width: 0; line-height: 1.8; font-size: .9em; }

.uw-reset { font-size: 1.35em; color: #f5a623; opacity: 0.8; cursor: pointer; line-height: 1; display: inline-flex; align-items: center; user-select: none; }
.uw-reset:hover { opacity: 1; }
.uw-users { margin: 6px 0 0; padding: 6px 8px; font-size: 0.85em; white-space: pre-wrap; background: var(--uw-code-bg, rgba(128,128,128,0.12)); border-radius: 4px; }
</style>

<div id="uw-message" style="min-height:1.4em;margin-bottom:4px;"></div>

<dl>
  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Status</dt>
  <dd>
    <strong id="uw-status" style="color:<?= $view['color'] ?>"><?= htmlspecialchars($view['status']) ?></strong>
    &nbsp;&nbsp;
    <button type="button" id="uw-toggle-btn" onclick="uwToggle()"><?= $st['running'] ? 'Stop Unwake' : 'Start Unwake' ?></button>
    <div id="uw-older-row" style="margin-top:6px;width:var(--uw-col);max-width:100%;<?= $view['older'] ? '' : 'display:none;' ?>">
      <span class="uw-warn" id="uw-older"><?= htmlspecialchars($view['older']) ?></span>
      <button type="button" onclick="uwUsers(this)" style="display:inline;margin:4px 0 0 6px;padding:2px 10px;min-width:0;">Who uses them?</button>
      <pre id="uw-users" class="uw-users" style="display:none;"></pre>
    </div>
    <div id="uw-restart-row" style="margin-top:6px;<?= ($st['running'] && $view['version'] && $view['version'] !== $version) ? '' : 'display:none;' ?>">
      <span class="uw-warn">Version <span id="uw-run-ver"><?= htmlspecialchars($view['version']) ?></span> is running, <?= htmlspecialchars($version) ?> is installed.</span>
      <input type="button" value="Restart Unwake" onclick="uwRestart()">
    </div>
    <div class="uw-help">Unwake sits on top of the user share file system (shfs) on <code>/mnt/user</code> and answers folder listings, name lookups and attribute reads from RAM.
      <br><strong>Stop</strong> takes it off at once: new access goes straight to shfs. Files, containers or VMs that still use it keep working through it until they close or restart; then it exits.
      <br>An update switches over in place: the new version takes the running one's place on the same mount, with its folders in RAM and every open file. Containers, VMs and SMB clients carry on, with a pause of a second or so. If that doesn't work, the old version keeps running and <em>Restart Unwake</em> appears: it tries the switch again, and only if that fails too, stops and starts Unwake - then what uses the old one (containers and VMs started before, open files) stays on it until it closes or restarts.</div>
  </dd>

  <dt id="uw-cachedirs-dt" style="<?= $cache_dirs && $st['running'] ? '' : 'display:none;' ?>">&nbsp;</dt>
  <dd id="uw-cachedirs-dd" style="<?= $cache_dirs && $st['running'] ? '' : 'display:none;' ?>">
    <div class="uw-col-text"><span class="uw-warn"><i class="fa fa-exclamation-triangle"></i> Dynamix Cache Directories is running. Unwake replaces it: turn it off under Settings &rarr; Folder Caching, so it stops reading your disks.</span></div>
  </dd>

  <dt id="uw-pressure-dt" style="<?= ($pressure !== null && $pressure !== 100) ? '' : 'display:none;' ?>">&nbsp;</dt>
  <dd id="uw-pressure-dd" style="<?= ($pressure !== null && $pressure !== 100) ? '' : 'display:none;' ?>">
    <div class="uw-col-text"><span class="uw-warn"><i class="fa fa-exclamation-triangle"></i> <code>vfs_cache_pressure</code> is <span id="uw-pressure"><?= (int)$pressure ?></span>, not the kernel default 100 - possibly left behind by Dynamix Cache Directories.
      With Unwake, a low value only keeps memory tied up that the kernel could use otherwise.</span>
    <input type="button" value="Set to 100" onclick="uwSetPressure()" style="display:inline;margin:4px 0 0 6px;padding:2px 10px;min-width:0;"></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Cache</dt>
  <dd>
    <div class="uw-fold-line" onclick="uwFold('cache')"><span id="uw-fold-caret-cache">&#9656;</span> <span id="uw-cache-line"><?= htmlspecialchars($view['cache'] ?: '-') ?></span></div>
    <div class="uw-fold-cache" style="display:none;" id="uw-cache-detail"><?= $view['cache_detail'] ?></div>
    <div class="uw-help">The folders and names Unwake holds in RAM, and how much memory that takes. Unlike the kernel's own caches, nothing can push this out of memory: a backup or a big copy doesn't make the next folder listing wake a disk. <em>Changes seen on the disks</em> counts new, renamed, deleted and changed files and folders, from any program, including the Mover and direct writes to <code>/mnt/diskN</code>.</div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Served from RAM</dt>
  <dd>
    <div class="uw-fold-line" onclick="uwFold('hits')"><span id="uw-fold-caret-hits">&#9656;</span> <span id="uw-hits-line"><?= htmlspecialchars($view['hits'] ?: '-') ?></span></div>
    <div class="uw-fold-hits" style="display:none;" id="uw-hits-detail"><?= $view['hits_detail'] ?></div>
    <div class="uw-help">Requests answered without asking shfs, since Unwake started. A request that goes to shfs may still be served from the kernel's caches; only the rest wake a disk. A folder is answered from RAM once it was listed through shfs once, or once every disk that may hold a part of it was read (see <em>Disks and Pools</em>). <em>Name lookups of missing files</em> are names like <code>desktop.ini</code> or <code>Thumbs.db</code> that SMB clients ask for in every folder.</div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Disks and Pools</dt>
  <dd>
    <div id="uw-disks"><?= $view['disks'] ?: '<span class="uw-muted">-</span>' ?></div>
    <div class="uw-help">Detected from Unraid's disk assignments: every array disk and pool that is part of the user shares. The dot shows whether a disk is spinning (green) or spun down. Unwake reads a disk's folders only while it spins anyway, at idle I/O priority, and never wakes one to do so. After that, it follows every change as it happens and doesn't read the folders again.</div>
  </dd>

  <dt class="uw-has-help uw-verify-row" onclick="uwToggleHelp(this)"<?= $view['verify'] ? '' : ' style="display:none;"' ?>>Verify</dt>
  <dd class="uw-verify-row"<?= $view['verify'] ? '' : ' style="display:none;"' ?>>
    <div class="uw-fold-line" onclick="uwFold('verify')"><span id="uw-fold-caret-verify">&#9656;</span> <span id="uw-verify-line"><?= htmlspecialchars($view['verify'] ?: '-') ?></span></div>
    <div class="uw-fold-verify" style="display:none;" id="uw-verify-detail"><?= $view['verify_detail'] ?></div>
    <div class="uw-help">Betas check the folders in RAM against the disks: a few folders per second, only while a disk spins anyway, a full pass at most every 12 hours. A difference counts only if a second look 5 seconds later shows the same. It is repaired in RAM and written to <code>/var/log/unwake-verify.log</code>, with what Unwake passed to shfs for that folder before. Stable versions verify with <code>VERIFY="yes"</code> in <code>unwake.cfg</code>.</div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Enable Unwake</dt>
  <dd>
    <select name="SERVICE" id="uw_SERVICE">
      <?= opt('disabled', $c['SERVICE'], 'No') ?>
      <?= opt('enabled',  $c['SERVICE'], 'Yes') ?>
    </select>
    <div class="uw-help">Master switch. Yes starts Unwake now, if the array is started, and with every array start. <strong>Default: No.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Read Folders of Spinning Disks</dt>
  <dd>
    <select name="HARVEST" id="uw_HARVEST">
      <?= opt('yes', $c['HARVEST'], 'Yes') ?>
      <?= opt('no',  $c['HARVEST'], 'No - only what is browsed') ?>
    </select>
    <div class="uw-help">While a disk is spinning anyway, read all of its folders into RAM once, at idle priority, so even folders nobody has opened yet won't wake it later. With No, Unwake learns only the folders that are listed through it. <strong>Default: Yes.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Read Files</dt>
  <dd>
    <select name="DATA_PATH" id="uw_DATA_PATH">
      <?= opt('direct', $c['DATA_PATH'], 'From their disk, when known') ?>
      <?= opt('shfs',   $c['DATA_PATH'], 'Through shfs') ?>
    </select>
    <div class="uw-help">Unwake never copies file data itself: the kernel reads and writes the files directly. <em>From their disk</em> reads a file that is known to exist on exactly one disk from that disk, skipping shfs. That keeps one copy less of the file in RAM, and so less pressure on the caches. Writes always go through shfs, which picks the disk. <strong>Default: From their disk.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Return Freed RAM</dt>
  <dd>
    <select name="RETURN_RAM" id="uw_RETURN_RAM">
      <?= opt('no',  $c['RETURN_RAM'], 'No') ?>
      <?= opt('yes', $c['RETURN_RAM'], 'Yes') ?>
    </select>
    <div class="uw-help">Besides the folders, Unwake keeps an entry for every name the kernel currently holds an inode for. A backup or a media scan through /mnt/user makes that many, and the kernel lets go of them only when it needs the RAM. With Yes, Unwake hands the memory freed then back to the system (at most every 5 minutes, after at least 100,000 names). With No, it keeps it for the next backup, which then gets it a little faster; the status shows how much is kept. Handing it back costs a few milliseconds each time and doesn't slow anything else down. <strong>Default: Yes.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Log Level</dt>
  <dd>
    <select name="LOG_LEVEL" id="uw_LOG_LEVEL">
      <?= opt('info',  $c['LOG_LEVEL'], 'Info') ?>
      <?= opt('debug', $c['LOG_LEVEL'], 'Debug (verbose - every change and cache miss)') ?>
    </select>
    <div class="uw-help"><strong>Default: Info.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Log Max Size</dt>
  <dd>
    <div class="uw-row">
      <input type="number" id="uw_LOG_MAX_SIZE_MB" name="LOG_MAX_SIZE_MB" min="1" value="<?= htmlspecialchars($c['LOG_MAX_SIZE_MB']) ?>" style="width:6em">
      <span>MB</span>
      <span class="uw-reset" onclick="uwReset('LOG_MAX_SIZE_MB')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="uw-help">Largest size of the log file. When it grows past this, the oldest entries are cut away down to <em>Log Trim Size</em>. <strong>Default: <?= htmlspecialchars($defaults['LOG_MAX_SIZE_MB']) ?>.</strong></div>
  </dd>

  <dt class="uw-has-help" onclick="uwToggleHelp(this)">Log Trim Size</dt>
  <dd>
    <div class="uw-row">
      <input type="number" id="uw_LOG_TRIM_SIZE_MB" name="LOG_TRIM_SIZE_MB" min="1" value="<?= htmlspecialchars($c['LOG_TRIM_SIZE_MB']) ?>" style="width:6em">
      <span>MB</span>
      <span class="uw-reset" onclick="uwReset('LOG_TRIM_SIZE_MB')" title="Reset to default">&#x21ba;</span>
    </div>
    <div class="uw-help">How much of the log is kept when it is cut: the newest entries, this many MB. Smaller than <em>Log Max Size</em>, so the log isn't cut again with every new entry. <strong>Default: <?= htmlspecialchars($defaults['LOG_TRIM_SIZE_MB']) ?>.</strong></div>
  </dd>
</dl>

<hr>

<dl>
  <dt class="uw-has-help" onclick="uwToggleHelp(this)" data-help-target="uw-shares-help">Shares in RAM</dt>
  <dd>
    <div class="uw-fold-line" onclick="uwFold('shares')"><span id="uw-fold-caret-shares">&#9656;</span> <span id="uw-shares-summary"></span></div>
  </dd>
<?php foreach ($shares as $name => $info): ?>
  <dt class="uw-share-name uw-fold-shares" style="display:none;"><?= htmlspecialchars($name) ?></dt>
  <dd class="uw-fold-shares" style="display:none;">
<?php if ($info['pool_only']): ?>
    <div class="uw-muted uw-pool-only" data-share="<?= htmlspecialchars($name) ?>" style="width:var(--uw-col);max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;"
      title="<?= htmlspecialchars($info['comment']) ?>"><?= htmlspecialchars(pool_only_text($name, $st)) ?></div>
<?php else: ?>
    <div class="uw-share-row">
      <label><input type="checkbox" class="uw-share-cb" name="SHARE_CACHE_<?= htmlspecialchars($name) ?>" <?= share_cached($name, $info, $c) ? 'checked' : '' ?>> <span class="uw-share-on"<?= share_cached($name, $info, $c) ? '' : ' style="display:none;"' ?>>keep in RAM</span></label>
      <span class="uw-muted" style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;"><?= htmlspecialchars($info['comment']) ?></span>
    </div>
<?php endif; ?>
  </dd>
<?php endforeach; ?>
  <dt class="uw-shares-help-row" style="display:none;">&nbsp;</dt>
  <dd class="uw-shares-help-row" style="display:none;"><div class="uw-help" id="uw-shares-help">Ticked shares have their folders kept in RAM by Unwake. Untick a share to pass it straight to shfs: Unwake then neither keeps it in RAM nor answers for it, as if Unwake weren't there. Shares whose settings put them on an SSD pool only are decided by Unwake: they can't wake a disk, so they're passed to shfs - unless an array disk still holds folders of them (left over from an older setup, or the Mover moving towards the pool after the pool ran full). Then shfs would check that disk on every listing, and Unwake keeps the share in RAM. Don't forget to click <strong>Apply</strong> below!</div></dd>

  <dt>&nbsp;</dt>
  <dd><button type="button" id="uw-apply-btn" onclick="uwSave()" disabled>Apply</button></dd>
</dl>

<br>

<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:6px;">
  <strong style="margin-bottom: -26px;">Activity Log</strong>
  <input type="button" value="Clear Log" onclick="uwClearLog()">
</div>
<textarea id="log-box" class="uw-bordered" readonly rows="18"
  style="width:100%;font-family:monospace;font-size:0.8em;resize:vertical;box-sizing:border-box;appearance:none;-webkit-appearance:none;"><?= htmlspecialchars($log_lines) ?: 'No log entries yet.' ?></textarea>

<div class="uw-footer">
  Made with ♥ by
  <img src="https://github.com/kaedinger.png" alt="kaedinger" class="uw-footer-avatar">
  <a href="https://kaedinger.de" target="_blank" rel="noopener">kaedinger</a>
  <span class="uw-footer-sep">·</span>
  <a href="https://github.com/kaedinger/unwake" target="_blank" rel="noopener">GitHub / Issues</a>
  <span class="uw-footer-sep">·</span>
  <a href="#" onclick="return false;" title="Support forum thread follows with the first public release">Support forum</a>
  <span class="uw-footer-sep">·</span>
  <a href="https://github.com/sponsors/kaedinger" target="_blank" rel="noopener">Sponsor ♥ - THANK YOU</a>
  <span class="uw-footer-sep">·</span>
  <a href="https://www.paypal.com/donate/?hosted_button_id=ZAR7SALQJDUNW" target="_blank" rel="noopener">Donate ♥ - THANK YOU</a>
  <div id="uw-channel" title="Installed: Unwake v<?= htmlspecialchars($version) ?>">
    <input type="checkbox" id="uw-ch-switch">
    <button type="button" id="uw-ch-go" disabled>Switch</button>
  </div>
</div>

<link type="text/css" rel="stylesheet" href="<?autov('/webGui/styles/jquery.switchbutton.css')?>">
<script src="<?autov('/webGui/javascript/jquery.switchbutton.js')?>"></script>
<script>
// Release channel switch: STABLE vX (i) [switch] BETA vY (i) [Switch/Update] - as in Unspin
(function () {
  var CH  = '/plugins/unwake/include/channel.php';
  var ins = { version: <?= json_encode($version) ?>, channel: <?= json_encode($channel) ?> };
  var dat = null;
  var sw  = $('#uw-ch-switch'), go = $('#uw-ch-go');

  function base(v) { return String(v).replace(/-beta$/, ''); }
  function newer(ch) { return !!(dat && dat[ch]) && base(dat[ch].version) > base(ins.version); }
  function label(name, ch) {
    var v = (dat && dat[ch]) ? dat[ch].version : (ch === ins.channel ? ins.version : 'unknown');
    var info = (dat && dat[ch])
      ? " <i class='fa fa-info-circle fa-fw blue-text' title='Release Notes' onclick=\"event.stopPropagation();openChanges('showchanges " + dat[ch].notes + "','Release Notes')\"></i>"
      : '';
    return name + " <span class='uw-ch-v" + (newer(ch) ? " red-text" : "") + "'>v" + v + "</span>" + info;
  }
  function target() { return sw.is(':checked') ? 'beta' : 'stable'; }
  function render() {
    sw.switchButton('option', 'off_label', label('STABLE', 'stable'));
    sw.switchButton('option', 'on_label',  label('BETA', 'beta'));
    var t = target(), own = t === ins.channel, upd = own && newer(t);
    go.text(upd ? 'Update' : 'Switch').prop('disabled', !dat || !dat[t] || (own && !upd));
  }
  function load(refresh) {
    $.get(CH, { action: 'versions', refresh: refresh ? 1 : 0 }, function (d) {
      dat = d; ins = d.installed; render();
    }, 'json').fail(function () { dat = null; render(); });
  }
  function note(t) {
    var s = dat.stable.version, b = dat.beta.version;
    if (t === 'beta') {
      return "This installs <b>v" + b + "</b> now.<br><br>" +
        "From now on, the Plugins page offers you beta versions as updates. " +
        "Betas get new features first, but are tested less than stable releases.<br><br>" +
        "Your settings stay as they are. You can switch back here at any time.";
    }
    var down = base(ins.version) > s;
    return "This installs <b>v" + s + "</b> now.<br><br>" +
      "From now on, the Plugins page only offers you stable releases as updates.<br><br>" +
      (down ? "<b>This is a downgrade.</b> Settings that only exist in the beta are dropped " +
              "the next time you save, and come back with their defaults if you switch to beta again.<br><br>" : "") +
      "You can switch back here at any time.";
  }
  go.click(function () {
    var t = target();
    if (!dat || !dat[t]) return;
    if (t === ins.channel) {
      if (newer(t)) openPlugin('plugin install ' + dat[t].url, 'Update unwake', 'unwake', 'uwChannelDone');
      return;
    }
    swal({ title: 'Switch to the ' + t + ' channel?', text: note(t), html: true, type: 'warning',
           showCancelButton: true, confirmButtonText: 'Switch', cancelButtonText: 'Cancel' }, function (ok) {
      if (!ok) return;
      setTimeout(function () {
        openPlugin('plugin install ' + dat[t].url + ' forced', 'Switch Unwake to the ' + t + ' channel', 'unwake', 'uwChannelDone');
      }, 300);
    });
  });
  window.uwChannelDone = function () {
    $.post(CH, { action: 'check' }).always(function () { location.reload(); });
  };
  sw.switchButton({ labels_placement: 'both', checked: ins.channel === 'beta', clear: false,
                    off_label: label('STABLE', 'stable'), on_label: label('BETA', 'beta') });
  sw.change(render);
  load(0);
})();
</script>

<script>
var UW_DEFAULTS = <?= json_encode($defaults) ?>;
var UW_INSTALLED = <?= json_encode($version) ?>;
(function () {
  var EXEC = '/plugins/unwake/include/exec.php';
  var applyBtn = document.getElementById('uw-apply-btn');
  var msgEl    = document.getElementById('uw-message');
  var lb       = document.getElementById('log-box');
  var running  = <?= json_encode($st['running']) ?>;
  if (lb) lb.scrollTop = lb.scrollHeight;

  document.querySelectorAll('[id^="uw_"], input.uw-share-cb').forEach(function (el) {
    el.addEventListener('change', function () { applyBtn.disabled = false; });
    el.addEventListener('input',  function () { applyBtn.disabled = false; });
  });
  // "keep in RAM" only next to a ticked box
  function shareLabel(cb) { cb.parentNode.querySelector('.uw-share-on').style.display = cb.checked ? '' : 'none'; }
  document.querySelectorAll('input.uw-share-cb').forEach(function (cb) {
    shareLabel(cb);
    cb.addEventListener('change', function () { shareLabel(cb); updateSharesSummary(); });
  });

  function showMsg(text, ok) {
    msgEl.innerHTML = '<span style="color:' + (ok ? '#4ade80' : '#f87171') + '">' + String(text).replace(/</g, '&lt;') + '</span>';
  }
  function post(data, cb) {
    $.post(EXEC, data, cb, 'json').fail(function (xhr) { showMsg('Error: ' + xhr.statusText, false); });
  }
  function setBusy(btn, text) { btn.disabled = true; btn.innerHTML = '<span class="uw-spinner"></span>' + text; }

  function apply(r) {
    running = r.running;
    var v = r.view, s = document.getElementById('uw-status');
    s.textContent = v.status; s.style.color = v.color;
    var btn = document.getElementById('uw-toggle-btn');
    btn.disabled = false; btn.innerHTML = running ? 'Stop Unwake' : 'Start Unwake';
    document.getElementById('uw-cache-line').textContent = v.cache || '-';
    document.getElementById('uw-cache-detail').innerHTML = v.cache_detail;
    document.getElementById('uw-hits-line').textContent = v.hits || '-';
    document.getElementById('uw-hits-detail').innerHTML = v.hits_detail;
    document.getElementById('uw-disks').innerHTML = v.disks || '<span class="uw-muted">-</span>';
    document.querySelectorAll('.uw-verify-row').forEach(function (el) { el.style.display = v.verify ? '' : 'none'; });
    document.getElementById('uw-verify-line').textContent = v.verify || '-';
    document.getElementById('uw-verify-detail').innerHTML = v.verify_detail || '';
    document.querySelectorAll('.uw-pool-only').forEach(function (el) {
      var t = v.pool_only && v.pool_only[el.dataset.share];
      el.textContent = t || 'SSD pool only - checked against the array disks while Unwake runs';
    });
    document.getElementById('uw-older-row').style.display = v.older ? '' : 'none';
    document.getElementById('uw-older').textContent = v.older;
    if (!v.older) document.getElementById('uw-users').style.display = 'none';
    var restart = running && v.version && v.version !== UW_INSTALLED;
    document.getElementById('uw-restart-row').style.display = restart ? '' : 'none';
    document.getElementById('uw-run-ver').textContent = v.version;
    var cd = r.cache_dirs && running;
    document.getElementById('uw-cachedirs-dt').style.display = cd ? '' : 'none';
    document.getElementById('uw-cachedirs-dd').style.display = cd ? '' : 'none';
    var lowp = r.pressure !== null && r.pressure !== undefined && r.pressure !== 100;   // only ever when it isn't 100
    document.getElementById('uw-pressure-dt').style.display = lowp ? '' : 'none';
    document.getElementById('uw-pressure-dd').style.display = lowp ? '' : 'none';
    if (lowp) document.getElementById('uw-pressure').textContent = r.pressure;
    if (lb && r.log !== undefined) {
      var atBottom = lb.scrollTop + lb.clientHeight >= lb.scrollHeight - 10;
      lb.value = r.log || 'No log entries yet.';
      if (atBottom) lb.scrollTop = lb.scrollHeight;
    }
  }
  function poll() { post({ action: 'poll' }, apply); }

  window.uwUsers = function (btn) {
    var pre = document.getElementById('uw-users');
    setBusy(btn, 'Looking...');
    post({ action: 'users' }, function (r) {
      btn.disabled = false; btn.innerHTML = 'Who uses them?';
      pre.textContent = r.text || 'Nothing found.';
      pre.style.display = '';
    });
  };

  window.uwFold = function (name, open) {
    var items = document.querySelectorAll('.uw-fold-' + name);
    if (!items.length) return;
    if (open === undefined) open = items[0].style.display === 'none';
    items.forEach(function (el) { el.style.display = open ? '' : 'none'; });
    document.getElementById('uw-fold-caret-' + name).innerHTML = open ? '&#9662;' : '&#9656;';
    try { localStorage.setItem('unwake-fold-' + name, open ? '1' : '0'); } catch (e) {}
  };
  ['cache', 'hits', 'verify', 'shares'].forEach(function (name) {
    try { if (localStorage.getItem('unwake-fold-' + name) === '1') uwFold(name, true); } catch (e) {}
  });

  function updateSharesSummary() {
    var cbs = document.querySelectorAll('input.uw-share-cb'), on = 0;
    cbs.forEach(function (cb) { if (cb.checked) on++; });
    var pool = document.querySelectorAll('.uw-pool-only').length;
    document.getElementById('uw-shares-summary').textContent = cbs.length + pool === 0 ? 'No shares found' :
      (cbs.length + pool) + (cbs.length + pool === 1 ? ' share: ' : ' shares: ') + on + ' in RAM, ' + (cbs.length - on) + ' passed to shfs' + (pool ? ', ' + pool + ' on SSD pools only' : '');
  }
  updateSharesSummary();

  var fields = ['SERVICE', 'HARVEST', 'DATA_PATH', 'RETURN_RAM', 'LOG_LEVEL', 'LOG_MAX_SIZE_MB', 'LOG_TRIM_SIZE_MB'];
  window.uwSave = function () {
    setBusy(applyBtn, 'Applying…');
    var data = { action: 'save' };
    fields.forEach(function (f) { var el = document.getElementById('uw_' + f); if (el) data[f] = el.value; });
    document.querySelectorAll('input.uw-share-cb').forEach(function (el) { data[el.name] = el.checked ? '1' : '0'; });
    post(data, function (r) {
      showMsg(r.message, r.ok);
      if (r.cfg) fields.forEach(function (f) { var el = document.getElementById('uw_' + f); if (el && r.cfg[f] !== undefined) el.value = r.cfg[f]; });
      apply(r);
      applyBtn.disabled = true; applyBtn.innerHTML = 'Apply';
    });
  };

  window.uwToggle = function () {
    var btn = document.getElementById('uw-toggle-btn');
    setBusy(btn, running ? 'Stopping…' : 'Starting…');
    post({ action: running ? 'stop' : 'start' }, function (r) { showMsg(r.message, r.ok); apply(r); });
  };

  window.uwRestart = function () {
    swal({ title: 'Restart Unwake?', type: 'warning', showCancelButton: true, confirmButtonText: 'Restart', cancelButtonText: 'Cancel',
           text: 'The installed version takes the running one\'s place on the same mount, with the folders in RAM and every open file - containers, VMs and SMB clients carry on. Only if that doesn\'t work, Unwake is stopped and started: then what uses the old one stays on it until it closes or restarts.' },
      function (ok) {
        if (!ok) return;
        setBusy(document.getElementById('uw-toggle-btn'), 'Restarting…');
        post({ action: 'restart' }, function (r) { showMsg(r.message, r.ok); apply(r); });
      });
  };

  window.uwSetPressure = function () {
    post({ action: 'set_pressure' }, function (r) { showMsg(r.message, r.ok); apply(r); });
  };

  window.uwClearLog = function () {
    post({ action: 'clear_log' }, function (r) { showMsg(r.message, r.ok); if (lb) lb.value = ''; });
  };

  window.uwReset = function (key) {
    var el = document.getElementById('uw_' + key);
    if (el) { el.value = UW_DEFAULTS[key]; applyBtn.disabled = false; }
  };

  window.uwToggleHelp = function (dt) {
    var help = null, dd = dt.nextElementSibling;
    if (dt.dataset.helpTarget) help = document.getElementById(dt.dataset.helpTarget);   // below the list, not folded with it
    else {
      while (dd && dd.tagName !== 'DD') dd = dd.nextElementSibling;
      if (dd) help = dd.querySelector('.uw-help');
    }
    if (!help) return;
    // A help below a list has a grid row of its own: out of the grid while closed, or its gap shows
    var rows = dt.dataset.helpTarget ? document.querySelectorAll('.' + dt.dataset.helpTarget + '-row') : [];
    var show = function (on) { rows.forEach(function (r) { r.style.display = on ? '' : 'none'; }); };
    if (help.classList.contains('open')) {
      help.classList.remove('open');
      if (rows.length) setTimeout(function () { if (!help.classList.contains('open')) show(false); }, 250);
    } else {
      show(true);
      void help.offsetHeight;   // let the row lay out first, so the panel still slides open
      help.classList.add('open');
    }
  };

  setInterval(poll, 10000);
}());
</script>
