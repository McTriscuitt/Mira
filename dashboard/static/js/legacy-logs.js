let _offset = 0;
let _filter = '';
let _exclude = '';
const PAGE = 50;

const _TZ = { timeZone: 'America/Chicago' };

function formatTimestamp(iso) {
  const d = new Date(iso);
  return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric', ..._TZ })
    + ' · '
    + d.toLocaleTimeString('en-US', _TZ);
}

function dateKey(iso) {
  const d = new Date(iso);
  return d.toLocaleDateString('en-US', { weekday: 'long', month: 'long', day: 'numeric', ..._TZ });
}

async function loadEntries() {
  const btn = document.getElementById('load-more-btn');
  btn.disabled = true;

  const params = new URLSearchParams({ limit: PAGE + _offset });
  if (_filter) params.set('search', _filter);
  if (_exclude) params.set('exclude', _exclude);
  const res = await fetch(`/api/log/recent?${params}`);
  const data = await res.json();
  if (!data) return;

  const list = document.getElementById('log-list');
  list.innerHTML = '';
  let lastDate = null;

  data.forEach(entry => {
    if (entry.timestamp) {
      const key = dateKey(entry.timestamp);
      if (key !== lastDate) {
        const sep = document.createElement('div');
        sep.className = 'log-date-sep';
        sep.textContent = key;
        list.appendChild(sep);
        lastDate = key;
      }
    }
    const el = document.createElement('div');
    el.className = 'log-entry';
    const time = document.createElement('span');
    time.className = 'log-time';
    time.textContent = formatTimestamp(entry.timestamp);
    const message = document.createElement('span');
    message.textContent = entry.message;
    el.append(time, message);
    list.appendChild(el);
  });

  _offset += PAGE;
  const exhausted = data.length < _offset;
  if (exhausted) btn.style.display = 'none';
  else btn.disabled = false;
}

document.getElementById('load-more-btn').addEventListener('click', loadEntries);

document.querySelectorAll('.filter-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    _filter = btn.dataset.search || '';
    _exclude = btn.dataset.exclude || '';
    _offset = 0;
    document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('filter-active'));
    btn.classList.add('filter-active');
    const loadBtn = document.getElementById('load-more-btn');
    loadBtn.style.display = '';
    loadBtn.disabled = true;
    loadEntries();
  });
});

loadEntries();
