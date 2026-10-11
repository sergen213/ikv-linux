(() => {
  'use strict';

  const element = (id) => document.getElementById(id);
  const numberFormat = new Intl.NumberFormat('tr-TR');
  let session = '';
  let sessionVersion = 0;
  let expiresAt = 0;
  let refreshTimer = 0;
  let expiryTimer = 0;
  let busy = false;
  let statusRequest = null;
  let accountLoaded = false;
  let characterLoaded = false;
  let selectedAccount = '';
  let selectedCharacter = '';
  let resetVisible = false;
  let zonesDirty = false;
  let latest = null; // last /v1/admin/status payload
  let settings = { xp_rate: 100, drop_rate: 100, regen_rate: 100, disabled_zones: [] };

  const text = (value) => value == null ? '—' : typeof value === 'object' ? JSON.stringify(value, null, 2) : String(value);
  const node = (tag, value, className) => {
    const result = document.createElement(tag);
    if (value !== undefined) result.textContent = text(value);
    if (className) result.className = className;
    return result;
  };
  const notice = (message, kind = 'info') => {
    resetVisible = false;
    element('feedback').textContent = message;
    element('feedback').dataset.kind = kind;
  };
  const timestamp = (value) => {
    if (typeof value === 'number' || /^\d+$/.test(String(value))) {
      const numeric = Number(value);
      return numeric < 100000000000 ? numeric * 1000 : numeric;
    }
    return Date.parse(value);
  };
  const date = (value) => {
    const time = timestamp(value);
    return Number.isFinite(time) ? new Date(time).toLocaleString('tr-TR') : text(value);
  };
  const count = (value) => typeof value === 'number' && Number.isFinite(value) ? numberFormat.format(value) : 'Bildirilmedi';
  const duration = (seconds) => {
    if (typeof seconds !== 'number' || !Number.isFinite(seconds)) return 'Bildirilmedi';
    const d = Math.floor(seconds / 86400), h = Math.floor(seconds % 86400 / 3600), m = Math.floor(seconds % 3600 / 60);
    return d ? `${d} g ${h} sa` : h ? `${h} sa ${m} dk` : `${m} dk ${Math.floor(seconds % 60)} sn`;
  };
  const positiveId = (value, label) => {
    const id = Number(value);
    if (!Number.isSafeInteger(id) || id <= 0) throw new Error(`${label} pozitif bir tamsayı olmalıdır.`);
    return id;
  };
  const sleep = (ms) => new Promise((resolve) => { setTimeout(resolve, ms); });

  function updateControls() {
    element('login-fields').disabled = busy;
    document.querySelectorAll('[data-operation]').forEach((button) => { button.disabled = busy || !session || button.dataset.unavailable === 'true'; });
    element('account-controls').disabled = busy || !session || !selectedAccount;
    element('character-controls').disabled = busy || !session || !selectedCharacter;
    document.querySelectorAll('.search input').forEach((input) => { input.disabled = busy; });
    element('dashboard').setAttribute('aria-busy', String(busy));
    element('login-form').setAttribute('aria-busy', String(busy));
  }

  const tables = ['account-results', 'character-results', 'log-results', 'backup-results', 'zone-summary', 'recent-commands', 'online-players', 'bot-results', 'zone-controls', 'command-history'];
  function clearSession(message, kind = 'info') {
    session = '';
    sessionVersion += 1;
    expiresAt = 0;
    latest = null;
    zonesDirty = false;
    clearTimeout(refreshTimer);
    clearTimeout(expiryTimer);
    selectedAccount = '';
    selectedCharacter = '';
    accountLoaded = false;
    characterLoaded = false;
    ['password', 'account-id', 'character-id', 'account-query', 'character-query', 'ban-reason', 'announce-text', 'control-message'].forEach((id) => { element(id).value = ''; });
    tables.forEach((id) => element(id).replaceChildren());
    ['move-zone', 'bot-zone'].forEach((id) => element(id).replaceChildren());
    ['m-online', 'm-bots', 'm-tick', 'm-uptime', 'm-memory', 'm-accounts', 'm-characters', 'm-sessions'].forEach((id) => { element(id).textContent = '—'; });
    element('world-log').textContent = '';
    element('validate-result').hidden = true;
    element('countdown-banner').hidden = true;
    element('identity').textContent = '';
    element('session-expiry').textContent = '';
    element('dashboard').hidden = true;
    element('login-panel').hidden = false;
    updateControls();
    notice(message, kind);
    element('username').focus();
  }

  async function fetchChecked(path, method, body, token, timeoutMs) {
    const version = sessionVersion;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), timeoutMs);
    try {
      const response = await fetch(path, {
        method, credentials: 'omit', cache: 'no-store', redirect: 'error', signal: controller.signal,
        headers: { Accept: 'application/json', ...(body === undefined ? {} : { 'Content-Type': 'application/json' }), ...(token ? { Authorization: `Bearer ${token}` } : {}) },
        ...(body === undefined ? {} : { body: JSON.stringify(body) })
      });
      if (version !== sessionVersion) throw new Error('Oturum kapandı; önceki isteğin sonucu kullanılmadı.');
      if (!response.ok) {
        let data = {};
        try { data = JSON.parse(await response.text()); } catch { /* status below */ }
        const message = data.error?.message || `İstek başarısız (HTTP ${response.status}).`;
        const code = data.error?.code ? ` [${data.error.code}]` : '';
        if (token && (response.status === 401 || (response.status === 403 && path === '/v1/admin/status'))) {
          clearSession(`Oturum veya yönetim yetkisi geçersiz. ${message}${code}`, 'error');
        }
        throw new Error(response.status === 403 ? `Yetkiniz bu işleme izin vermiyor. ${message}${code}` : `${message}${code}`);
      }
      return response;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error(`İstek ${timeoutMs / 1000} saniyede tamamlanmadı. İşlemin uygulanıp uygulanmadığını yenileyerek kontrol edin; otomatik tekrar gönderilmedi.`);
      if (error instanceof TypeError) throw new Error('HTTPS servisine erişilemedi. Ağ bağlantısını ve sunucu sertifikasını kontrol edin; işlemi tekrar göndermeden önce durumu yenileyin.');
      throw error;
    } finally { clearTimeout(timeout); }
  }

  async function request(path, method = 'GET', body, token = session) {
    const response = await fetchChecked(path, method, body, token, path === '/v1/admin/backups' && method === 'POST' ? 310000 : 15000);
    const raw = await response.text();
    try { return raw ? JSON.parse(raw) : {}; }
    catch { throw new Error(`Sunucu geçerli JSON döndürmedi (HTTP ${response.status}).`); }
  }

  async function perform(action) {
    if (busy) return;
    notice('İşlem yürütülüyor…');
    busy = true;
    updateControls();
    try { await action(); }
    catch (error) { notice(error.message, 'error'); }
    finally { busy = false; updateControls(); }
  }

  function emptyRow(target, columns, message) {
    const cell = node('td', message, 'empty');
    cell.colSpan = columns;
    const row = node('tr');
    row.append(cell);
    target.append(row);
  }

  function detail(value, label = 'Kayıt ayrıntıları') {
    const container = node('details');
    container.append(node('summary', label), node('pre', value));
    return container;
  }

  function actionButton(label, title, handler, className = 'secondary') {
    const button = node('button', label, className);
    button.type = 'button';
    button.title = title;
    button.dataset.operation = '';
    button.disabled = busy;
    button.addEventListener('click', () => { if (!busy) handler(); });
    return button;
  }

  const worldStatus = (world) => world && typeof world.status === 'object' && world.status ? world.status : {};
  const currentWorld = (data) => {
    const worlds = Array.isArray(data?.worlds) ? data.worlds : [];
    return worlds.find((world) => world.online) || worlds[0] || null;
  };
  const zoneNames = () => (Array.isArray(worldStatus(currentWorld(latest)).zones) ? worldStatus(currentWorld(latest)).zones : []).map((zone) => String(zone.zone));

  function resultText(command) {
    if (!command.consumed) return 'bekliyor';
    if (command.result == null) return 'işleniyor';
    const result = command.result;
    if (result.ok === false) return `HATA: ${text(result.error)}`;
    const extra = Object.entries(result).filter(([key]) => key !== 'ok').map(([key, value]) => `${key}=${typeof value === 'object' ? JSON.stringify(value) : value}`);
    return extra.length ? `tamam (${extra.join(', ')})` : 'tamam';
  }

  function fillZoneSelect(id, withDefault) {
    const select = element(id);
    const previous = select.value;
    const names = zoneNames();
    select.replaceChildren();
    if (withDefault) { const option = node('option', 'Varsayılan doğma bölgesi'); option.value = ''; select.append(option); }
    names.forEach((name) => { const option = node('option', name); option.value = name; select.append(option); });
    if ([...select.options].some((option) => option.value === previous)) select.value = previous;
  }

  function renderStatus(data) {
    latest = data;
    const world = currentWorld(data);
    const status = worldStatus(world);
    const online = Array.isArray(status.online) ? status.online : [];
    const bots = Array.isArray(status.bots) ? status.bots : [];
    const zones = Array.isArray(status.zones) ? status.zones : [];
    const live = Boolean(world && world.online);
    element('m-online').textContent = live ? count(status.players) : 'Çevrimdışı';
    element('m-bots').textContent = live ? count(status.bots_total ?? bots.length) : '—';
    element('m-tick').textContent = live && typeof status.tick_ms === 'number' ? `${status.tick_ms.toFixed(2)} ms` : '—';
    element('m-uptime').textContent = live ? duration(status.uptime_seconds) : '—';
    element('m-memory').textContent = live && typeof status.memory_kib === 'number' ? `${(status.memory_kib / 1024).toFixed(1)} MiB` : '—';
    element('m-accounts').textContent = count(data.accounts);
    element('m-characters').textContent = count(data.characters);
    element('m-sessions').textContent = count(data.sessions);
    element('world-summary').textContent = world
      ? `${world.instance} · ${live ? 'çevrimiçi' : 'ÇEVRİMDIŞI'} · son bildirim ${date(world.updated_at)} · tick ${count(status.tick)} · bağlantı ${count(status.connections)} · port ${text(status.world_port)}`
      : 'Kayıtlı dünya örneği yok. ikv-server çalışıyor mu?';

    const banner = element('countdown-banner');
    banner.hidden = !(live && status.countdown);
    if (live && status.countdown) banner.textContent = `Sunucu ${status.countdown.seconds} saniye içinde ${status.countdown.command === 'restart' ? 'yeniden başlatılacak' : 'kapatılacak'}. İptal için Dünya sekmesini kullanın.`;

    const zoneSummary = element('zone-summary');
    zoneSummary.replaceChildren();
    if (!zones.length) emptyRow(zoneSummary, 5, 'Bölge verisi yok.');
    zones.forEach((zone) => {
      const row = node('tr');
      row.append(node('td', zone.zone), node('td', zone.players), node('td', zone.bots ?? 0), node('td', zone.npcs ?? '—'), node('td', zone.disabled ? 'kapalı' : 'açık', zone.disabled ? 'bad' : 'ok'));
      zoneSummary.append(row);
    });

    const commands = Array.isArray(data.commands) ? data.commands : [];
    const recent = element('recent-commands');
    const history = element('command-history');
    recent.replaceChildren();
    history.replaceChildren();
    if (!commands.length) { emptyRow(recent, 3, 'Henüz komut yok.'); emptyRow(history, 4, 'Henüz komut yok.'); }
    commands.forEach((command, index) => {
      const outcome = resultText(command);
      const kind = command.result?.ok === false ? 'bad' : command.result ? 'ok' : '';
      if (index < 10) {
        const row = node('tr');
        row.append(node('td', command.id), node('td', command.command), node('td', outcome, kind));
        recent.append(row);
      }
      const row = node('tr');
      row.append(node('td', `${command.id} / ${date(command.created_at)}`), node('td', command.command), node('td', JSON.stringify(command.arguments)), node('td', outcome, kind));
      history.append(row);
    });

    const players = element('online-players');
    players.replaceChildren();
    if (!online.length) emptyRow(players, 6, live ? 'Çevrimiçi oyuncu yok.' : 'Dünya çevrimdışı.');
    online.forEach((player) => {
      const row = node('tr');
      row.append(node('td', player.id), node('td', `${text(player.name)} (#${text(player.character_id)})${player.admin ? ' · yönetici' : ''}${player.muted ? ' · susturulmuş' : ''}`),
        node('td', player.level), node('td', `${text(player.hp)}/${text(player.max_hp)}`), node('td', `${text(player.zone)} (${text(player.x)}, ${text(player.y)})`));
      const actions = node('td');
      actions.append(
        actionButton('İncele', 'Inspect saved character state', () => { void perform(() => inspect(player)); }),
        actionButton('Işınla', 'Fill the teleport form with this character', () => { element('move-character').value = player.character_id; element('move-zone').value = player.zone; element('move-x').focus(); }),
        actionButton('Hedef yap', 'Use as the summon target', () => { element('summon-to').value = player.character_id; element('move-character').focus(); }),
        actionButton('At', 'Kick from the world', () => { void kick(player.character_id, player.name); }, 'danger'));
      row.append(actions);
      players.append(row);
    });

    const botTable = element('bot-results');
    botTable.replaceChildren();
    if (!bots.length) emptyRow(botTable, 7, live ? 'Bot yok.' : 'Dünya çevrimdışı.');
    bots.forEach((bot) => {
      const row = node('tr');
      row.append(node('td', bot.id), node('td', bot.name), node('td', `${text(bot.behaviour)}${bot.leader ? ` · lider ${bot.leader}` : ''}`), node('td', `${text(bot.level)} / ${text(bot.hp)}`),
        node('td', `${text(bot.zone)} (${text(bot.x)}, ${text(bot.y)})`), node('td', bot.state));
      const actions = node('td');
      actions.append(actionButton('Kaldır', 'Despawn this bot', () => {
        void worldCommand('/v1/admin/bots', { action: 'despawn', ids: [bot.id] }, `Bot ${bot.id} kaldırma`, `Bot ${text(bot.name)} (${bot.id}) kaldırılsın mı?`);
      }, 'danger'));
      row.append(actions);
      botTable.append(row);
    });

    fillZoneSelect('move-zone', false);
    fillZoneSelect('bot-zone', true);
    if (!zonesDirty) {
      const controls = element('zone-controls');
      controls.replaceChildren();
      if (!zones.length) emptyRow(controls, 6, 'Bölge verisi yok.');
      zones.forEach((zone) => {
        const row = node('tr');
        const toggle = node('td');
        const box = document.createElement('input');
        box.type = 'checkbox';
        box.checked = !zone.disabled;
        box.dataset.zone = zone.zone;
        box.title = `Zone ${zone.zone} open for entry`;
        box.setAttribute('aria-label', `${zone.zone} açık`);
        box.addEventListener('change', () => { zonesDirty = true; });
        toggle.append(box);
        const respawn = node('td');
        respawn.append(actionButton('NPC canlandır', 'Respawn dead NPCs in this zone', () => {
          void worldCommand('/v1/admin/world', { command: 'respawn', zone: zone.zone }, `${zone.zone} NPC canlandırma`);
        }));
        row.append(toggle, node('td', zone.zone), node('td', zone.players), node('td', zone.bots ?? 0), node('td', zone.npcs ?? '—'), respawn);
        controls.append(row);
      });
    }

    const worldLog = Array.isArray(status.logs) ? status.logs : [];
    element('world-log').textContent = worldLog.length
      ? worldLog.slice().reverse().map((entry) => `tick ${entry.tick}  oyuncu ${entry.player}  ${entry.action || ''} ${entry.result || ''}${entry.code ? ` (${entry.code})` : ''}`).join('\n')
      : 'Günlük kaydı yok.';

    element('clock-state').textContent = typeof status.clock_hour === 'number'
      ? `Oyun saati: ${String(Math.floor(status.clock_hour)).padStart(2, '0')}:${String(Math.floor(status.clock_hour % 1 * 60)).padStart(2, '0')} (yeniden başlatmada yapılandırılmış saate döner)`
      : 'Bu dünya yapılandırmasında gün saati yok.';

    const logs = element('log-results');
    logs.replaceChildren();
    const entries = Array.isArray(data.logs) ? data.logs : [];
    if (!entries.length) emptyRow(logs, 5, 'Yönetim kaydı yok.');
    entries.forEach((entry) => {
      const row = node('tr');
      row.append(node('td', `${text(entry.id)} / ${date(entry.created_at)}`), node('td', entry.action), node('td', entry.actor_id), node('td', entry.target_id));
      const details = node('td');
      details.append(node('pre', entry.details));
      row.append(details);
      logs.append(row);
    });
    updateControls();
  }

  async function refreshStatus() {
    if (statusRequest) {
      try { await statusRequest; } catch { /* A new request still checks the current state. */ }
    }
    if (!session) throw new Error('Yönetim oturumu kapalı.');
    const pending = request('/v1/admin/status');
    statusRequest = pending;
    try { renderStatus(await pending); }
    finally { if (statusRequest === pending) statusRequest = null; }
  }

  function scheduleRefresh() {
    clearTimeout(refreshTimer);
    if (!session) return;
    refreshTimer = setTimeout(async () => {
      if (!busy && !document.hidden) {
        try { await refreshStatus(); }
        catch (error) { notice(error.message, 'error'); }
      }
      scheduleRefresh();
    }, 10000);
  }

  // Commands are queued for the world's 1 s heartbeat; poll status until the world records the result.
  async function awaitCommand(id) {
    for (let attempt = 0; attempt < 12 && session; attempt += 1) {
      await sleep(800);
      await refreshStatus();
      const command = (latest?.commands || []).find((entry) => entry.id === id);
      if (command && command.result != null) return command;
    }
    return null;
  }

  async function worldCommand(path, body, label, confirmation) {
    await perform(async () => {
      if (confirmation && !window.confirm(confirmation)) { notice('İşlem iptal edildi.'); return; }
      const queued = await request(path, 'POST', body);
      notice(`${label}: komut #${queued.command_id} kuyruğa alındı; dünyanın yanıtı bekleniyor…`);
      const command = await awaitCommand(queued.command_id);
      if (!command) { notice(`${label}: komut #${queued.command_id} henüz işlenmedi. Dünya çalışıyor mu? Günlükler sekmesinden izleyin.`, 'error'); return; }
      notice(`${label}: ${resultText(command)}`, command.result.ok === false ? 'error' : 'success');
    });
  }

  async function kick(characterId, name) {
    return worldCommand('/v1/admin/world', { command: 'kick', character_id: positiveId(characterId, 'Karakter ID') }, `Karakter ${characterId} atma`,
      `${name ? `${name} ` : ''}(karakter ${characterId}) oyundan atılsın mı? Tekrar bağlanmasını engellemek için hesabı yasaklayın.`);
  }

  function selectRecord(kind, id) {
    notice(`${kind === 'account' ? 'Hesap' : 'Karakter'} seçildi: ${id}`);
    if (kind === 'account') selectedAccount = id;
    else selectedCharacter = id;
    element(`${kind}-id`).value = id;
    element(`${kind}-results`).querySelectorAll('tr').forEach((row) => { row.setAttribute('aria-selected', String(row.dataset.id === id)); });
    updateControls();
  }

  async function search(kind) {
    const plural = kind === 'account' ? 'accounts' : 'characters';
    const data = await request(`/v1/admin/${plural}?search=${encodeURIComponent(element(`${kind}-query`).value.trim())}`);
    const records = Array.isArray(data[plural]) ? data[plural] : [];
    const target = element(`${kind}-results`);
    const selected = kind === 'account' ? selectedAccount : selectedCharacter;
    target.replaceChildren();
    if (!records.length) emptyRow(target, 4, 'Eşleşen kayıt yok.');
    records.forEach((record) => {
      const id = record.id == null ? '' : String(record.id);
      const validId = typeof record.id === 'number' && Number.isSafeInteger(record.id) && record.id > 0;
      const row = node('tr');
      row.dataset.id = id;
      row.setAttribute('aria-selected', String(id !== '' && id === selected));
      const label = kind === 'account' ? `${text(record.username)}${record.admin ? ' · yönetici' : ''}${record.plus ? ' · Plus' : ''}${record.kasa ? ' · Kasa Erişimi' : ''}${record.banned ? ' · YASAKLI' : ''}`
        : `${text(record.name)} · hesap ${text(record.account_id)} · sv. ${text(record.state?.level)}${record.moderation?.muted ? ' · susturulmuş' : ''}`;
      row.append(node('td', record.id), node('td', label));
      const details = node('td');
      details.append(detail(record));
      const selection = node('td');
      const button = node('button', validId ? (id === selected ? 'Seçili' : 'Seç') : 'ID güvenli değil', 'secondary');
      button.type = 'button';
      button.title = 'Select';
      button.dataset.operation = '';
      button.dataset.unavailable = String(!validId);
      button.disabled = !validId || busy;
      button.setAttribute('aria-label', `${kind === 'account' ? 'Hesap' : 'Karakter'} ${id} seç`);
      button.addEventListener('click', () => { if (!busy && validId) selectRecord(kind, id); });
      selection.append(button);
      row.append(details, selection);
      target.append(row);
    });
    if (!records.some((record) => String(record.id) === selected)) {
      if (kind === 'account') selectedAccount = '';
      else selectedCharacter = '';
      element(`${kind}-id`).value = '';
    }
    element(`${kind}-results-label`).textContent = `${records.length} kayıt döndü (API sonuçları)`;
    if (kind === 'account') accountLoaded = true;
    else characterLoaded = true;
    updateControls();
    return records;
  }

  async function inspect(player) {
    element('character-query').value = player.name;
    const records = await search('character');
    const id = String(player.character_id);
    if (!records.some((record) => String(record.id) === id)) throw new Error(`Karakter ${id} kaydı bulunamadı.`);
    selectRecord('character', id);
    const row = element('character-results').querySelector(`tr[data-id="${CSS.escape(id)}"]`);
    row?.querySelector('details')?.setAttribute('open', '');
    row?.scrollIntoView({ block: 'nearest' });
    notice(`Karakter ${player.name} (${id}) kaydı açıldı.`, 'success');
  }

  async function refreshBackups() {
    try {
      const data = await request('/v1/admin/backups');
      const backups = Array.isArray(data.backups) ? data.backups : [];
      const target = element('backup-results');
      target.replaceChildren();
      if (!backups.length) emptyRow(target, 3, 'Henüz yedek yok.');
      backups.forEach((backup) => {
        const row = node('tr');
        const action = node('td');
        action.append(actionButton('İndir', 'Download this SQL backup', () => { void perform(() => download(String(backup.name))); }));
        row.append(node('td', backup.name), node('td', count(backup.size)), action);
        target.append(row);
      });
      element('backup-state').textContent = `${backups.length} yedek · ${new Date().toLocaleTimeString('tr-TR')}`;
    } catch (error) {
      element('backup-state').textContent = `Yedek listesi güncellenemedi. ${error.message}`;
      throw error;
    }
  }

  async function download(name) {
    if (!/^ikv-[0-9a-f-]+\.sql$/.test(name)) throw new Error('Geçersiz yedek adı.');
    const response = await fetchChecked(`/v1/admin/backups/${encodeURIComponent(name)}`, 'GET', undefined, session, 120000);
    const url = URL.createObjectURL(await response.blob());
    const link = document.createElement('a');
    link.href = url;
    link.download = name;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
    notice(`${name} indirildi. Dosya parola/oturum özetleri içerir; korumalı ve şifreli saklayın.`, 'success');
  }

  async function refreshSettings() {
    const data = await request('/v1/admin/settings');
    if (data.settings && typeof data.settings === 'object') settings = { ...settings, ...data.settings };
    element('xp-rate').value = settings.xp_rate;
    element('drop-rate').value = settings.drop_rate;
    element('regen-rate').value = settings.regen_rate;
    element('settings-state').textContent = `Kayıtlı ayarlar: tecrübe %${settings.xp_rate}, ganimet %${settings.drop_rate}, yenilenme %${settings.regen_rate}; kapalı bölgeler: ${settings.disabled_zones.length ? settings.disabled_zones.join(', ') : 'yok'}.`;
  }

  async function saveSettings(next, label) {
    await worldCommand('/v1/admin/settings', next, label);
    await refreshSettings();
  }

  async function refreshAll() {
    await refreshStatus();
    const actions = [refreshBackups(), refreshSettings()];
    if (accountLoaded) actions.push(search('account'));
    if (characterLoaded) actions.push(search('character'));
    const results = await Promise.allSettled(actions);
    const errors = results.filter((result) => result.status === 'rejected').map((result) => result.reason.message);
    if (errors.length) throw new Error(errors.join('\n'));
  }

  async function afterMutation(message, sensitive = false) {
    if (!session) return;
    const version = sessionVersion;
    try {
      await refreshAll();
      if (session && version === sessionVersion) { notice(message, 'success'); resetVisible = sensitive; }
    } catch (error) {
      if (session && version === sessionVersion) { notice(`${message}\nYenileme tamamlanamadı: ${error.message}`, 'error'); resetVisible = sensitive; }
    }
  }

  function showTab(name) {
    document.querySelectorAll('[role="tab"]').forEach((tab) => {
      const selected = tab.id === `tab-${name}`;
      tab.setAttribute('aria-selected', String(selected));
      tab.tabIndex = selected ? 0 : -1;
      element(tab.getAttribute('aria-controls')).hidden = !selected;
    });
  }
  const tabs = [...document.querySelectorAll('[role="tab"]')];
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => showTab(tab.id.slice(4)));
    tab.addEventListener('keydown', (event) => {
      if (event.key !== 'ArrowRight' && event.key !== 'ArrowLeft') return;
      const next = tabs[(index + (event.key === 'ArrowRight' ? 1 : tabs.length - 1)) % tabs.length];
      showTab(next.id.slice(4));
      next.focus();
    });
  });
  showTab(/^#tab-([a-z]+)$/.test(window.location.hash) && element(window.location.hash.slice(1)) ? window.location.hash.slice(5) : 'overview');

  element('login-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const username = element('username').value.trim();
    const password = element('password').value;
    element('password').value = '';
    void perform(async () => {
      try {
        const login = await request('/v1/login', 'POST', { username, password }, '');
        if (typeof login.session !== 'string' || !login.session) throw new Error('Sunucu geçerli oturum tokenı döndürmedi.');
        session = login.session;
        sessionVersion += 1;
        expiresAt = timestamp(login.expires_at);
        if (!Number.isFinite(expiresAt) || expiresAt <= Date.now()) throw new Error('Oturum süresi geçersiz veya dolmuş.');
        renderStatus(await request('/v1/admin/status'));
        if (!session) return;
        element('identity').textContent = `Yetkili hesap: ${username} · Yönetim erişimi sunucu tarafından doğrulandı`;
        element('session-expiry').textContent = `Oturum sonu: ${date(login.expires_at)}`;
        element('login-panel').hidden = true;
        element('dashboard').hidden = false;
        expiryTimer = setTimeout(() => clearSession('Oturum süresi doldu. Yeniden giriş yapın.', 'error'), Math.min(expiresAt - Date.now(), 2147483647));
        scheduleRefresh();
        notice('Yönetim erişimi doğrulandı. Veriler yükleniyor…', 'success');
      } catch (error) {
        clearSession(`Giriş tamamlanamadı: ${error.message}`, 'error');
        return;
      }
      const results = await Promise.allSettled([search('account'), search('character'), refreshBackups(), refreshSettings()]);
      const errors = results.filter((result) => result.status === 'rejected').map((result) => result.reason.message);
      if (!session) return;
      notice(errors.length ? `Yönetim erişimi doğrulandı; bazı veriler alınamadı:\n${errors.join('\n')}` : 'Yönetim erişimi doğrulandı. Canlı veriler alındı.', errors.length ? 'error' : 'success');
      if (session) element('refresh').focus();
    });
  });

  element('logout').addEventListener('click', async () => {
    const token = session;
    clearSession('Yerel oturum kapatıldı; sunucudaki oturum iptal ediliyor.');
    try { await request('/v1/logout', 'POST', {}, token); notice('Oturum sunucuda da kapatıldı.', 'success'); }
    catch (error) { notice(`Yerel token silindi. Sunucuda iptal doğrulanamadı; oturum süresi dolana kadar geçerli olabilir. ${error.message}`, 'error'); }
  });

  for (const kind of ['account', 'character']) {
    element(`${kind}-search`).addEventListener('submit', (event) => {
      event.preventDefault();
      void perform(async () => { await search(kind); notice('Arama tamamlandı.', 'success'); });
    });
  }

  element('account-action').addEventListener('submit', (event) => {
    event.preventDefault();
    const action = event.submitter?.value;
    const id = selectedAccount;
    if (!id || !['ban', 'unban', 'reset', 'plus', 'unplus', 'kasa', 'unkasa'].includes(action)) return;
    void perform(async () => {
      positiveId(id, 'Hesap ID');
      const reason = element('ban-reason').value.trim();
      const seconds = Number(element('ban-duration').value);
      if (action === 'ban' && (!reason || new TextEncoder().encode(reason).length > 512)) throw new Error('Yasaklama gerekçesi 1–512 UTF-8 bayt olmalıdır; Türkçe karakterler birden fazla bayt kullanabilir.');
      if (action === 'ban' && (!Number.isSafeInteger(seconds) || seconds < 0 || seconds > 31536000)) throw new Error('Yasak süresi 0–31536000 saniye arasında bir tamsayı olmalıdır; 0 süresiz yasaktır.');
      const prompt = action === 'ban' ? `Hesap ${id} yasaklansın mı?\nGerekçe: ${reason}\nSüre: ${seconds === 0 ? 'Süresiz' : `${seconds} saniye`}` : action === 'reset' ? `Hesap ${id} için parola sıfırlama tokenı üretilecek. Tokenı hesap sahibine güvenli bir kanaldan elle teslim etmelisiniz. Onaylıyor musunuz?`
        : action === 'plus' ? `Hesap ${id} Plus Paket sahibi yapılsın mı? (satıcıya 1/3 fiyat, 3 kat Şan, %10 Karizma kaybı)` : action === 'unplus' ? `Hesap ${id} Plus Paket yetkisi kaldırılsın mı?`
        : action === 'kasa' ? `Hesap ${id} Kasa Erişimi sahibi yapılsın mı? (Kasamı Göster ile her yerden banka)` : action === 'unkasa' ? `Hesap ${id} Kasa Erişimi kaldırılsın mı?` : `Hesap ${id} yasağı kaldırılsın mı?`;
      if (!window.confirm(prompt)) { notice('İşlem iptal edildi.'); return; }
      const result = await request(`/v1/admin/accounts/${encodeURIComponent(id)}/${action}`, 'POST', action === 'ban' ? { reason, duration_seconds: seconds } : {});
      if (action === 'reset') {
        if (typeof result.token !== 'string' || !result.token) throw new Error('Sunucu sıfırlama tokenı döndürmedi; yeniden üretmeden önce durumu kontrol edin.');
        await afterMutation(`Hesap ${id} için sıfırlama tokenı üretildi. Güvenli bir kanaldan elle teslim edin.\nTOKEN: ${result.token}\nSon kullanım: ${date(result.expires_at)}\nSonraki işlemde bu token ekrandan silinir.`, true);
      } else await afterMutation(`Hesap ${id}: ${{ ban: 'yasak uygulandı', unban: 'yasak kaldırıldı', plus: 'Plus Paket verildi', unplus: 'Plus Paket kaldırıldı', kasa: 'Kasa Erişimi verildi', unkasa: 'Kasa Erişimi kaldırıldı' }[action]}.`);
    });
  });

  element('character-action').addEventListener('submit', (event) => {
    event.preventDefault();
    const action = event.submitter?.value;
    const id = selectedCharacter;
    if (!id || !['mute', 'unmute', 'kick'].includes(action)) return;
    if (action === 'kick') { void kick(id); return; }
    void perform(async () => {
      const seconds = action === 'mute' ? Number(element('mute-duration').value) : 0;
      if (!Number.isSafeInteger(seconds) || seconds < (action === 'mute' ? 1 : 0) || seconds > 604800) throw new Error('Susturma süresi 1–604800 saniye arasında bir tamsayı olmalıdır.');
      if (!window.confirm(`Karakter ${id}: ${action === 'mute' ? `${seconds} saniye susturulsun mu?` : 'susturma kaldırılsın mı?'}`)) { notice('İşlem iptal edildi.'); return; }
      await request('/v1/admin/moderation', 'POST', { character_id: positiveId(id, 'Karakter ID'), action, duration_seconds: seconds });
      await afterMutation(`Karakter ${id}: ${action === 'mute' ? 'susturma uygulandı' : 'susturma kaldırıldı'}.`);
    });
  });

  element('move-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const command = event.submitter?.value;
    try {
      const character = positiveId(element('move-character').value, 'Karakter ID');
      if (command === 'summon') {
        const to = positiveId(element('summon-to').value, 'Hedef karakter ID');
        void worldCommand('/v1/admin/world', { command, character_id: character, to_character_id: to }, `Karakter ${character} → ${to} yanına çağırma`);
        return;
      }
      if (command !== 'teleport') return;
      const body = { command, character_id: character, zone: element('move-zone').value };
      const x = element('move-x').value.trim(), y = element('move-y').value.trim();
      if (x !== '' || y !== '') {
        if (x === '' || y === '') throw new Error('X ve Y birlikte girilmeli veya ikisi de boş bırakılmalıdır.');
        body.x = Number(x); body.y = Number(y);
        if (!Number.isSafeInteger(body.x) || !Number.isSafeInteger(body.y)) throw new Error('Koordinatlar tamsayı olmalıdır.');
      }
      void worldCommand('/v1/admin/world', body, `Karakter ${character} ışınlama`);
    } catch (error) { notice(error.message, 'error'); }
  });

  element('bot-spawn').addEventListener('submit', (event) => {
    event.preventDefault();
    const body = { action: 'spawn', behaviour: element('bot-behaviour').value, count: Number(element('bot-count').value) };
    if (element('bot-zone').value) body.zone = element('bot-zone').value;
    if (element('bot-level').value.trim()) body.level = Number(element('bot-level').value);
    if (!Number.isSafeInteger(body.count) || body.count < 1 || body.count > 50) { notice('Adet 1–50 arasında olmalıdır.', 'error'); return; }
    if (body.level !== undefined && (!Number.isSafeInteger(body.level) || body.level < 1 || body.level > 100)) { notice('Seviye 1–100 arasında olmalıdır.', 'error'); return; }
    void worldCommand('/v1/admin/bots', body, `${body.count} bot oluşturma`);
  });
  element('bot-despawn-zone').addEventListener('click', () => {
    const zone = element('bot-zone').value;
    if (!zone) { notice('Önce bir bölge seçin.', 'error'); return; }
    void worldCommand('/v1/admin/bots', { action: 'despawn', zone }, `${zone} botlarını kaldırma`, `${zone} bölgesindeki tüm botlar kaldırılsın mı?`);
  });
  element('bot-despawn-all').addEventListener('click', () => {
    void worldCommand('/v1/admin/bots', { action: 'despawn', all: true }, 'Tüm botları kaldırma', 'Tüm botlar kaldırılsın mı?');
  });

  element('announce-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const message = element('announce-text').value.trim();
    if (!message || new TextEncoder().encode(message).length > 256) { notice('Duyuru 1–256 UTF-8 bayt olmalıdır.', 'error'); return; }
    void worldCommand('/v1/admin/world', { command: 'announce', message }, 'Duyuru', `Tüm oyunculara duyurulsun mu?\n${message}`);
  });

  element('control-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const command = event.submitter?.value;
    if (command === 'cancel') { void worldCommand('/v1/admin/control', { command }, 'Geri sayım iptali'); return; }
    if (!['restart', 'shutdown'].includes(command)) return;
    const delay = Number(element('control-delay').value);
    if (!Number.isSafeInteger(delay) || delay < 0 || delay > 3600) { notice('Geri sayım 0–3600 saniye olmalıdır.', 'error'); return; }
    const body = { command, delay_seconds: delay };
    const message = element('control-message').value.trim();
    if (message) body.message = message;
    void worldCommand('/v1/admin/control', body, command === 'restart' ? 'Yeniden başlatma' : 'Kapatma',
      `Dünya servisi ${delay} saniye sonra ${command === 'restart' ? 'yeniden başlatılacak' : 'durdurulacak'}. Bağlı oyuncular uyarılacak ve bağlantıları kesilecek. Onaylıyor musunuz?`);
  });

  element('clock-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const hour = Number(element('clock-hour').value);
    if (!Number.isSafeInteger(hour) || hour < 0 || hour > 23) { notice('Saat 0–23 olmalıdır.', 'error'); return; }
    void worldCommand('/v1/admin/world', { command: 'clock', hour }, `Saat ${hour}:00`);
  });

  element('validate-config').addEventListener('click', () => {
    void perform(async () => {
      const queued = await request('/v1/admin/world', 'POST', { command: 'validate_config' });
      notice('Yapılandırma denetimi kuyruğa alındı…');
      const command = await awaitCommand(queued.command_id);
      if (!command) throw new Error('Dünya denetim sonucunu bildirmedi. Dünya çalışıyor mu?');
      const output = element('validate-result');
      output.hidden = false;
      output.textContent = JSON.stringify(command.result, null, 2);
      notice(command.result.ok ? 'Yapılandırma geçerli (uygulanmadı).' : `Yapılandırma geçersiz: ${text(command.result.error)}`, command.result.ok ? 'success' : 'error');
    });
  });

  element('zones-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const disabled = [...element('zone-controls').querySelectorAll('input[type="checkbox"]')].filter((box) => !box.checked).map((box) => box.dataset.zone);
    void saveSettings({ xp_rate: settings.xp_rate, drop_rate: settings.drop_rate, regen_rate: settings.regen_rate, disabled_zones: disabled }, `Bölge ayarları (kapalı: ${disabled.length ? disabled.join(', ') : 'yok'})`)
      .then(() => { zonesDirty = false; });
  });
  element('respawn-all').addEventListener('click', () => { void worldCommand('/v1/admin/world', { command: 'respawn' }, 'Tüm NPC canlandırma'); });

  element('settings-form').addEventListener('submit', (event) => {
    event.preventDefault();
    const rates = ['xp-rate', 'drop-rate', 'regen-rate'].map((id) => Number(element(id).value));
    if (rates.some((value) => !Number.isSafeInteger(value) || value < 10 || value > 1000)) { notice('Oranlar 10–1000 arasında tamsayı yüzde olmalıdır.', 'error'); return; }
    void saveSettings({ xp_rate: rates[0], drop_rate: rates[1], regen_rate: rates[2], disabled_zones: settings.disabled_zones }, 'Oran ayarları');
  });

  element('backup-create').addEventListener('click', () => {
    void perform(async () => {
      if (!window.confirm('Veritabanının yeni yedeği sunucuda oluşturulsun mu? İşlem disk alanı ve veritabanı kaynakları kullanır.')) { notice('İşlem iptal edildi.'); return; }
      const result = await request('/v1/admin/backups', 'POST', {});
      await afterMutation(`Yedek oluşturuldu: ${text(result.name)} (${count(result.size)} bayt).`);
    });
  });
  element('backup-refresh').addEventListener('click', () => { void perform(async () => { await refreshBackups(); notice('Yedek listesi güncellendi.', 'success'); }); });
  element('refresh').addEventListener('click', () => { void perform(async () => { await refreshAll(); notice('Canlı veriler güncellendi.', 'success'); }); });
  const clearTransientToken = () => { if (resetVisible) notice('Sıfırlama tokenı ekrandan silindi.'); };
  document.addEventListener('click', (event) => { if (event.target.closest('button')) clearTransientToken(); }, true);
  document.addEventListener('submit', clearTransientToken, true);
  document.addEventListener('input', clearTransientToken, true);
  document.addEventListener('visibilitychange', () => { if (!document.hidden && session) scheduleRefresh(); });
  window.addEventListener('pagehide', () => clearSession('Sayfadan ayrıldığınız için yerel oturum silindi.'));

  emptyRow(element('account-results'), 4, 'Arama yapılmadı.');
  emptyRow(element('character-results'), 4, 'Arama yapılmadı.');
  emptyRow(element('backup-results'), 3, 'Liste henüz yüklenmedi.');
  updateControls();
  if (window.location.protocol !== 'https:') {
    element('login-fields').disabled = true;
    notice('Güvenli giriş için HTTPS yönetim adresini kullanın: https://127.0.0.1:7444/admin (veya yapılandırılmış özel ağ adresi).', 'error');
  }
})();
