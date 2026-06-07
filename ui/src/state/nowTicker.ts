import { useEffect } from 'react'
import { useSetAtom } from 'jotai'
import { nowAtom } from './atoms'

/**
 * Mount once at app root. Updates nowAtom at 1Hz. Widgets that need
 * "X seconds ago" derive from secondsAgoAtom. Widgets that don't
 * care about time don't subscribe, so they don't re-render.
 */
export function useNowTicker() {
  const tick = useSetAtom(nowAtom)
  useEffect(() => {
    const id = setInterval(() => tick(), 1000)
    return () => clearInterval(id)
  }, [tick])
}