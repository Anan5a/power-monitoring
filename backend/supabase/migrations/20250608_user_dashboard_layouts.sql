-- Per-user dashboard layout persistence (added 2026-06-08 redesign)
create table if not exists public.user_dashboard_layouts (
  user_id uuid primary key references auth.users(id) on delete cascade,
  doc jsonb not null,
  updated_at timestamptz not null default now()
);

-- RLS: a user can read and write only their own layout row
alter table public.user_dashboard_layouts enable row level security;

create policy "users read own layout"
  on public.user_dashboard_layouts
  for select
  using (auth.uid() = user_id);

create policy "users upsert own layout"
  on public.user_dashboard_layouts
  for insert
  with check (auth.uid() = user_id);

create policy "users update own layout"
  on public.user_dashboard_layouts
  for update
  using (auth.uid() = user_id)
  with check (auth.uid() = user_id);

grant select, insert, update on public.user_dashboard_layouts to authenticated;
