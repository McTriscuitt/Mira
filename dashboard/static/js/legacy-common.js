// Shared by the old dashboard's pages. Logging out needs a POST with the CSRF token,
// so a link on another site can't log you out.

function csrfToken() {
  return document.querySelector('meta[name="csrf-token"]').content;
}

document.querySelectorAll('[data-logout]').forEach(function (button) {
  button.addEventListener('click', async function () {
    await fetch('/api/logout', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': csrfToken() },
      body: '{}',
    });
    window.location.href = '/';
  });
});
