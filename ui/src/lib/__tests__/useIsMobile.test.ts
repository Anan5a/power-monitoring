import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { renderHook } from '@testing-library/react'
import { useIsMobile } from '../useIsMobile'

function mockMatchMedia(matches: boolean) {
  return vi.fn().mockImplementation(() => ({
    matches,
    media: '',
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    dispatchEvent: vi.fn(),
  }))
}

describe('useIsMobile', () => {
  beforeEach(() => vi.stubGlobal('matchMedia', mockMatchMedia(false)))
  afterEach(() => vi.unstubAllGlobals())

  it('returns true for narrow viewport', () => {
    vi.stubGlobal('matchMedia', mockMatchMedia(true))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(true)
  })

  it('returns false for wide viewport', () => {
    vi.stubGlobal('matchMedia', mockMatchMedia(false))
    const { result } = renderHook(() => useIsMobile())
    expect(result.current).toBe(false)
  })
})
