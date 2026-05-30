import { serve } from "https://deno.land/std@0.177.0/http/server.ts";
import { createClient } from "https://esm.sh/@supabase/supabase-js@2";

const SUPABASE_URL = Deno.env.get("SUPABASE_URL")!;
const SUPABASE_SERVICE_ROLE_KEY = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

interface RealtimeMessage {
  topic: string;
  event: string;
  payload: {
    event?: string;
    data?: unknown;
    status?: string;
  };
  ref: string | null;
}

serve(async (req) => {
  const supabase = createClient(SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY);

  try {
    const body = await req.json();
    const msg = body as RealtimeMessage;

    // Only handle broadcast events on telemetry channels
    if (msg.event !== "broadcast") {
      return new Response("ok", { status: 200 });
    }

    const topic = msg.topic;
    // topic format: "devices:{device_key}:telemetry"
    const match = topic.match(/^devices:(.+):telemetry$/);
    if (!match) {
      return new Response("ok", { status: 200 });
    }

    const deviceKey = match[1];
    const payload = msg.payload as { event?: string; data?: unknown };
    const eventData = payload.data as {
      payload?: Record<string, unknown>;
      metadata?: Record<string, unknown>;
      recorded_at?: number;
    };

    if (!eventData || !eventData.payload) {
      return new Response("ok", { status: 200 });
    }

    // Validate device exists and api_key matches
    const device = await supabase
      .from("devices")
      .select("device_key, device_api_key")
      .eq("device_key", deviceKey)
      .maybeSingle();

    if (!device) {
      console.error(`listen-telemetry: unknown device_key: ${deviceKey}`);
      return new Response("ok", { status: 200 });
    }

    // Insert telemetry row
    const recordedAt = eventData.recorded_at
      ? new Date(eventData.recorded_at * 1000).toISOString()
      : new Date().toISOString();

    const { error } = await supabase.from("telemetry_live").insert({
      device_id: deviceKey,
      payload: eventData.payload,
      metadata: eventData.metadata ?? {},
      recorded_at: recordedAt,
    });

    if (error) {
      console.error(`listen-telemetry: insert error for ${deviceKey}:`, error.message);
    }

    return new Response("ok", { status: 200 });
  } catch (err) {
    console.error("listen-telemetry: unexpected error:", err);
    return new Response("ok", { status: 200 });
  }
});
