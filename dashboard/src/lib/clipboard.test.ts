import { afterEach, describe, expect, it, vi } from 'vitest'
import { copyText } from './clipboard'

/** Installs (or removes) the pieces of the browser clipboard API. */
function setClipboardApi(writeText?: (text: string) => Promise<void>) {
  Object.defineProperty(navigator, 'clipboard', {
    value: writeText ? { writeText } : undefined,
    configurable: true,
  })
}

function setExecCommand(result: boolean) {
  const exec = vi.fn().mockReturnValue(result)
  Object.defineProperty(document, 'execCommand', {
    value: exec,
    configurable: true,
    writable: true,
  })
  return exec
}

describe('copyText', () => {
  afterEach(() => {
    setClipboardApi(undefined)
    document.body.innerHTML = ''
    vi.restoreAllMocks()
  })

  it('uses the async clipboard API when available', async () => {
    const writeText = vi.fn().mockResolvedValue(undefined)
    setClipboardApi(writeText)
    await expect(copyText('hello')).resolves.toBe(true)
    expect(writeText).toHaveBeenCalledWith('hello')
  })

  it('falls back to execCommand when the clipboard API is missing', async () => {
    // Plain-HTTP LAN deployment: navigator.clipboard is undefined.
    setClipboardApi(undefined)
    const exec = setExecCommand(true)
    await expect(copyText('fallback')).resolves.toBe(true)
    expect(exec).toHaveBeenCalledWith('copy')
    // The temporary textarea must not be left behind.
    expect(document.querySelector('textarea')).toBeNull()
  })

  it('falls back when the clipboard API rejects', async () => {
    setClipboardApi(vi.fn().mockRejectedValue(new Error('not allowed')))
    const exec = setExecCommand(true)
    await expect(copyText('rejected')).resolves.toBe(true)
    expect(exec).toHaveBeenCalledWith('copy')
  })

  it('reports failure when neither strategy works', async () => {
    setClipboardApi(undefined)
    setExecCommand(false)
    await expect(copyText('nope')).resolves.toBe(false)
  })
})
