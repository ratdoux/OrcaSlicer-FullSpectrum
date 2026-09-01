// Snapmaker LAN printer firmware may reject X-Request-Id during CORS preflight.
// Keep the diagnostics header for cloud traffic and suppress it only for
// XMLHttpRequests sent directly to private/local network hosts.
(() => {
  if (XMLHttpRequest.prototype.__snapmakerLanRequestGuardInstalled) return;

  const xhrOpen = XMLHttpRequest.prototype.open;
  const xhrSetRequestHeader = XMLHttpRequest.prototype.setRequestHeader;

  const isLanHost = (url) => {
    try {
      const hostname = new URL(url, window.location.href).hostname
        .replace(/^\[|\]$/g, '')
        .toLowerCase();
      const ipv4 = hostname.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);

      if (hostname === 'localhost' || hostname.endsWith('.local')) return true;
      if (hostname.includes(':') && /^(fc|fd|fe[89ab])/.test(hostname)) return true;
      if (!ipv4 || ipv4.slice(1).some((octet) => Number(octet) > 255)) return false;

      const first = Number(ipv4[1]);
      const second = Number(ipv4[2]);
      return first === 10 ||
        first === 127 ||
        (first === 169 && second === 254) ||
        (first === 172 && second >= 16 && second <= 31) ||
        (first === 192 && second === 168);
    } catch (_) {
      return false;
    }
  };

  XMLHttpRequest.prototype.open = function (method, url, ...args) {
    this.__snapmakerLanRequest = isLanHost(url);
    return xhrOpen.call(this, method, url, ...args);
  };

  XMLHttpRequest.prototype.setRequestHeader = function (name, value) {
    if (this.__snapmakerLanRequest && String(name).toLowerCase() === 'x-request-id') return;
    return xhrSetRequestHeader.call(this, name, value);
  };

  Object.defineProperty(XMLHttpRequest.prototype, '__snapmakerLanRequestGuardInstalled', {
    value: true,
  });
})();
