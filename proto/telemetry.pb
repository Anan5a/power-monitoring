
ª
telemetry.protonanopb.proto"\
TelemetryDevice
id (Bí?Rid
fw (Bí?Rfw
	uptime_ms (RuptimeMs":
TelemetryWifi
rssi (Rrssi
ip (Bí?Rip"à
TelemetryChannel
ch (Rch
V (RV
I (RI
P (RP
	energy_Wh (RenergyWh

charge_mAh (R	chargeMAh"ç
TelemetrySwitch
idx (Ridx
type (Rtype
state (Rstate
	auto_mode (RautoMode!
rule_tripped (RruleTripped"ı
TelemetryBattery
ch (Rch

profile_id (R	profileId
	chemistry (R	chemistry
rated_Ah (RratedAh
soc_pct (RsocPct
V (RV
I (RI(
cumulative_Ah_in (RcumulativeAhIn*
cumulative_Ah_out	 (RcumulativeAhOut4
equivalent_full_cycles
 (RequivalentFullCycles
soh_pct (RsohPct
soh_samples (R
sohSamples"H
TelemetryLogMeta
entries (Rentries
overflow (Roverflow"ö
TelemetrySnapshot
ts (Rts
ts_ms (RtsMs%
schema_version (RschemaVersion
schema (Bí?Rschema(
device (2.TelemetryDeviceRdevice"
wifi (2.TelemetryWifiRwifi#
channel_count (RchannelCount!
switch_count (RswitchCount#
num_batteries	 (RnumBatteries4
channels
 (2.TelemetryChannelBí?Rchannels3
switches (2.TelemetrySwitchBí?Rswitches2
battery (2.TelemetryBatteryBí?Rbattery#
log (2.TelemetryLogMetaRlog
	heap_free (RheapFreebproto3