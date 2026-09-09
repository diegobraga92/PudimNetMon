import { describe, expect, it } from 'vitest'
import {
  decodeInstallConfig,
  decoratedInstallerFilename,
  encodeInstallConfig,
} from './installConfig'

describe('installConfig tokens', () => {
  it('round-trips collector, node and interval', () => {
    const token = encodeInstallConfig({
      collector: 'collector.lan:50051',
      node: 'web-01',
      interval: 5000,
    })
    // base64url charset is filename-safe on Windows and POSIX.
    expect(token).toMatch(/^[A-Za-z0-9_-]+$/)
    expect(decodeInstallConfig(token)).toEqual({
      collector: 'collector.lan:50051',
      node: 'web-01',
      interval: 5000,
    })
  })

  it('omits empty node and encodes collector only', () => {
    const token = encodeInstallConfig({ collector: '10.0.0.5:50051' })
    expect(decodeInstallConfig(token)).toEqual({
      collector: '10.0.0.5:50051',
    })
  })

  it('rejects malformed tokens', () => {
    expect(decodeInstallConfig('not-base64!!')).toBeNull()
    expect(decodeInstallConfig('')).toBeNull()
  })

  it('mirrors the collector Content-Disposition filename decoration', () => {
    const token = encodeInstallConfig({ collector: 'c:50051' })
    expect(decoratedInstallerFilename('PudimNetMon-Agent-Setup-0.1.0.exe', token)).toBe(
      `PudimNetMon-Agent-Setup-0.1.0-cfg-${token}.exe`,
    )
    expect(
      decoratedInstallerFilename('pudimnetmon-agent-0.1.0-linux-amd64.run', token),
    ).toBe(`pudimnetmon-agent-0.1.0-linux-amd64-cfg-${token}.run`)
    expect(
      decoratedInstallerFilename('pudimnetmon-agent-0.1.0-linux-amd64.tar.gz', token),
    ).toBe(`pudimnetmon-agent-0.1.0-linux-amd64-cfg-${token}.tar.gz`)
  })
})
