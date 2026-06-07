import type { Store } from 'jotai/vanilla/store'
import { supabase } from '../../lib/supabase'
import { layoutAtom } from '../atoms'
import { defaultLayoutAtom } from '../layout'
import type { LayoutDoc } from '../../lib/types'

interface SavedLayout {
  user_id: string
  doc: LayoutDoc
  updated_at: string
}

export async function loadLayout(store: Store, userId: string): Promise<LayoutDoc> {
  const { data, error } = await supabase
    .from('user_dashboard_layouts')
    .select('doc, updated_at')
    .eq('user_id', userId)
    .maybeSingle()
  if (error || !data) {
    const fallback = store.get(defaultLayoutAtom)
    store.set(layoutAtom, fallback)
    return fallback
  }
  const saved = (data as SavedLayout).doc
  const doc = saved?.version === 1 ? saved : store.get(defaultLayoutAtom)
  store.set(layoutAtom, doc)
  return doc
}

export async function saveLayout(store: Store, userId: string, doc: LayoutDoc): Promise<void> {
  store.set(layoutAtom, doc)
  await supabase
    .from('user_dashboard_layouts')
    .upsert({ user_id: userId, doc, updated_at: new Date().toISOString() })
}

export function resetLayout(store: Store): LayoutDoc {
  const doc = store.get(defaultLayoutAtom)
  store.set(layoutAtom, doc)
  return doc
}