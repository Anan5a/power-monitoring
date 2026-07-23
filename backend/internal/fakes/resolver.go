// internal/fakes/resolver.go — Returns canned devices for pipeline tests.

package fakes

import (
	"context"

	"github.com/Anan5a/iot-platform/internal"
)

type StubResolver struct {
	Device *internal.Device
	Err    error
}

func (r *StubResolver) Resolve(_ context.Context, deviceKey string) (*internal.Device, error) {
	if r.Err != nil {
		return nil, r.Err
	}
	if r.Device != nil {
		return r.Device, nil
	}
	return &internal.Device{
		DeviceKey:  deviceKey,
		DeviceType: "power_monitor_v2",
		IsActive:   true,
	}, nil
}
