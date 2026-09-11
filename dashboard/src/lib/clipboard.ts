/**
 * Copies text to the clipboard, returning false when both strategies fail so
 * callers can tell the user instead of failing silently.
 *
 * `navigator.clipboard` only exists in a secure context (HTTPS or localhost),
 * but the dashboard is normally served over plain HTTP on a LAN address, where
 * it is undefined. The legacy `execCommand('copy')` path still works there, so
 * it is used as the fallback.
 */
export async function copyText(text: string): Promise<boolean> {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text)
      return true
    } catch {
      // Permission denied or an insecure context: fall through to execCommand.
    }
  }
  try {
    const ta = document.createElement('textarea')
    ta.value = text
    ta.setAttribute('readonly', '')
    ta.style.position = 'fixed'
    ta.style.opacity = '0'
    document.body.appendChild(ta)
    try {
      ta.select()
      // jsdom (tests) has no execCommand implementation.
      return typeof document.execCommand === 'function' && document.execCommand('copy')
    } finally {
      document.body.removeChild(ta)
    }
  } catch {
    return false
  }
}
