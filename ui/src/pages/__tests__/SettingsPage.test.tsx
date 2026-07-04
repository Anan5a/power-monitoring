import { describe, it, expect, vi, beforeEach } from 'vitest'
import { render, screen, fireEvent, waitFor } from '@testing-library/react'
import { BrowserRouter } from 'react-router-dom'
import { Provider, createStore } from 'jotai'
import { devicesLoadingAtom } from '../../state/atoms'

// Supabase is mocked as a generic chainable. We need enough hooks to
// cover the SettingsPage's initial device load (devices + device_channels)
// and the legacy direct-insert paths (set_battery_profile, set_channel_group).
const insertMock = vi.fn().mockResolvedValue({ error: null })
const fromMock = vi.fn((table: string) => {
  if (table === 'devices') {
    return {
      select: vi.fn(() => ({
        order: vi.fn().mockResolvedValue({
          data: [{ device_key: 'dev-1', device_name: 'Test Device' }],
          error: null,
        }),
      })),
    }
  }
  if (table === 'device_channels') {
    return {
      select: vi.fn(() => ({
        eq: vi.fn(() => ({
          maybeSingle: vi.fn().mockResolvedValue({ data: null, error: null }),
        })),
      })),
    }
  }
  if (table === 'relay_states') {
    return {
      select: vi.fn(() => ({
        eq: vi.fn(() => ({
          order: vi.fn().mockResolvedValue({ data: [], error: null }),
        })),
      })),
    }
  }
  return {
    select: vi.fn().mockReturnThis(),
    eq: vi.fn().mockReturnThis(),
    order: vi.fn().mockResolvedValue({ data: [], error: null }),
    insert: insertMock,
    maybeSingle: vi.fn().mockResolvedValue({ data: null, error: null }),
  }
})

const channelMock = {
  on: vi.fn().mockReturnThis(),
  subscribe: vi.fn().mockReturnThis(),
}

vi.mock('../../lib/supabase', () => ({
  supabase: {
    from: (table: string) => fromMock(table),
    auth: {
      getSession: vi.fn().mockResolvedValue({ data: { session: { user: { id: 'u' } } } }),
    },
    channel: vi.fn(() => channelMock),
    removeChannel: vi.fn(),
  },
}))

// Mock the unified deviceCommands module. Each helper is its own vi.fn so
// tests can assert calls per tab. The factory must not reference outer
// top-level variables (vitest hoists vi.mock).
vi.mock('../../lib/deviceCommands', () => ({
  setWifi: vi.fn().mockResolvedValue(undefined),
  setMqtt: vi.fn().mockResolvedValue(undefined),
  setHttp: vi.fn().mockResolvedValue(undefined),
  setSupabase: vi.fn().mockResolvedValue(undefined),
  setCalibration: vi.fn().mockResolvedValue(undefined),
  setInvertCurrent: vi.fn().mockResolvedValue(undefined),
  setShunt: vi.fn().mockResolvedValue(undefined),
  setVoltRatio: vi.fn().mockResolvedValue(undefined),
  setResistors: vi.fn().mockResolvedValue(undefined),
  setVirtualChannel: vi.fn().mockResolvedValue(undefined),
  setChannelName: vi.fn().mockResolvedValue(undefined),
  setBattery: vi.fn().mockResolvedValue(undefined),
  setBatteryLegacy: vi.fn().mockResolvedValue(undefined),
  setBatteryProfile: vi.fn().mockResolvedValue(undefined),
  setRelayEnergized: vi.fn().mockResolvedValue(undefined),
  reboot: vi.fn().mockResolvedValue(undefined),
  factoryReset: vi.fn().mockResolvedValue(undefined),
  calibrateBaseline: vi.fn().mockResolvedValue(undefined),
  resetCoulomb: vi.fn().mockResolvedValue(undefined),
  setSwitch: vi.fn().mockResolvedValue(undefined),
  resetCalibration: vi.fn().mockResolvedValue(undefined),
  resetBattery: vi.fn().mockResolvedValue(undefined),
  deleteBatteryProfile: vi.fn().mockResolvedValue(undefined),
  setPin: vi.fn().mockResolvedValue(undefined),
}))

// Pull the mocked module back so we can read each helper's call history.
// Done in a separate import so the vi.mock factory has been resolved.
import * as dc from '../../lib/deviceCommands'
import SettingsPage from '../SettingsPage'

function renderWithStore() {
  const store = createStore()
  store.set(devicesLoadingAtom, false)
  return render(
    <Provider store={store}>
      <BrowserRouter>
        <SettingsPage />
      </BrowserRouter>
    </Provider>,
  )
}

// The page begins in a `loadingDevices` state and renders a "Loading..."
// stub. Tests must wait for the first real UI element (the network tab's
// "Save WiFi" button) to appear before interacting.
async function renderAndWaitForReady() {
  const view = renderWithStore()
  await waitFor(() => {
    expect(screen.getByText('Save WiFi')).toBeInTheDocument()
  }, { timeout: 3000 })
  return view
}

beforeEach(() => {
  for (const fn of Object.values(dc)) {
    if (typeof fn === 'function' && 'mockClear' in fn) {
      ;(fn as ReturnType<typeof vi.fn>).mockClear()
    }
  }
  insertMock.mockClear()
  // Reset the fromMock default behavior in case a test mutated it
  fromMock.mockClear()
})

describe('SettingsPage sendCommand migration', () => {
  // The network tab exposes several "Save" buttons. WiFi is the default
  // tab so it renders without any extra click.
  it('WiFi Save calls setWifi with ssid/pass', async () => {
    await renderAndWaitForReady()
    // The WiFi form is on the default 'network' tab
    const ssidInput = screen.getAllByPlaceholderText('WiFi name')[0] as HTMLInputElement
    const passInput = screen.getAllByPlaceholderText('Password')[0] as HTMLInputElement
    fireEvent.change(ssidInput, { target: { value: 'TestNet' } })
    fireEvent.change(passInput, { target: { value: 'secret' } })
    fireEvent.click(screen.getByText('Save WiFi'))

    await waitFor(() => {
      expect(dc.setWifi).toHaveBeenCalledWith('dev-1', 'TestNet', 'secret')
    })
  })

  it('MQTT Save calls setMqtt with broker/port/topic', async () => {
    await renderAndWaitForReady()
    const broker = screen.getByPlaceholderText('192.168.1.100') as HTMLInputElement
    const port = screen.getByPlaceholderText('1883') as HTMLInputElement
    const topic = screen.getByPlaceholderText('power-monitor/data') as HTMLInputElement
    fireEvent.change(broker, { target: { value: 'mqtt.local' } })
    fireEvent.change(port, { target: { value: '1884' } })
    fireEvent.change(topic, { target: { value: 'sensors/data' } })
    fireEvent.click(screen.getByText('Save MQTT'))

    await waitFor(() => {
      expect(dc.setMqtt).toHaveBeenCalledWith('dev-1', 'mqtt.local', 1884, 'sensors/data')
    })
  })

  it('HTTP Save calls setHttp with url/token/enabled', async () => {
    await renderAndWaitForReady()
    const url = screen.getByPlaceholderText('https://...') as HTMLInputElement
    const token = screen.getByPlaceholderText('Bearer token') as HTMLInputElement
    fireEvent.change(url, { target: { value: 'https://example.com/hook' } })
    fireEvent.change(token, { target: { value: 'tok' } })
    fireEvent.click(screen.getByText('Save HTTP'))

    await waitFor(() => {
      expect(dc.setHttp).toHaveBeenCalledWith('dev-1', 'https://example.com/hook', 'tok', true)
    })
  })

  it('Save Shunts calls setShunt per channel', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('sensors'))
    for (const ch of [0, 1, 2]) {
      const input = document.getElementById(`shunt-${ch}`) as HTMLInputElement
      fireEvent.change(input, { target: { value: '0.0005' } })
    }
    fireEvent.click(screen.getByText('Save Shunts'))

    await waitFor(() => {
      expect(dc.setShunt).toHaveBeenCalledTimes(3)
      expect(dc.setShunt).toHaveBeenNthCalledWith(1, 'dev-1', 0, 0.0005)
      expect(dc.setShunt).toHaveBeenNthCalledWith(2, 'dev-1', 1, 0.0005)
      expect(dc.setShunt).toHaveBeenNthCalledWith(3, 'dev-1', 2, 0.0005)
    })
  })

  it('Save Ratios calls setVoltRatio per channel', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('sensors'))
    for (const ch of [0, 1, 2]) {
      const input = document.getElementById(`vratio-${ch}`) as HTMLInputElement
      fireEvent.change(input, { target: { value: '3.5' } })
    }
    fireEvent.click(screen.getByText('Save Ratios'))

    await waitFor(() => {
      expect(dc.setVoltRatio).toHaveBeenCalledTimes(3)
      expect(dc.setVoltRatio).toHaveBeenNthCalledWith(1, 'dev-1', 0, 3.5)
    })
  })

  it('Save Resistors calls setResistors with r_high/r_low', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('sensors'))
    const rh0 = document.getElementById('rhigh-0') as HTMLInputElement
    const rl0 = document.getElementById('rlow-0') as HTMLInputElement
    fireEvent.change(rh0, { target: { value: '100000' } })
    fireEvent.change(rl0, { target: { value: '30000' } })
    // The sensor tab has "Save Shunts" and "Save Ratios" buttons; the
    // resistor rows have buttons that say exactly "Save". Use a regex
    // anchored to "Save" to pick up the row buttons.
    const saves = screen.getAllByText(/^Save$/)
    fireEvent.click(saves[0])

    await waitFor(() => {
      expect(dc.setResistors).toHaveBeenCalledWith('dev-1', 0, 100000, 30000)
    })
  })

  it('Virtual Channel Save calls setVirtualChannel', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('virtual'))
    // Click the first Save button in the virtual tab
    const saves = screen.getAllByText('Save')
    fireEvent.click(saves[0])

    await waitFor(() => {
      expect(dc.setVirtualChannel).toHaveBeenCalledWith(
        'dev-1',
        0,
        expect.objectContaining({
          voltage_src: expect.any(Number),
          voltage_idx: expect.any(Number),
          current_src: expect.any(Number),
          current_idx: expect.any(Number),
        }),
      )
    })
  })

  it('Channel Name Save calls setChannelName', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('names'))
    const input = screen.getAllByPlaceholderText('e.g. Solar Panel')[0] as HTMLInputElement
    fireEvent.change(input, { target: { value: 'Solar' } })
    const saves = screen.getAllByText('Save')
    fireEvent.click(saves[0])

    await waitFor(() => {
      expect(dc.setChannelName).toHaveBeenCalledWith('dev-1', 0, 'Solar')
    })
  })

  it('Battery Save Basic calls setBatteryLegacy (capacity_mAh + initial_soc_pct)', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('batteries'))
    const cap = screen.getAllByPlaceholderText('5000')[0] as HTMLInputElement
    fireEvent.change(cap, { target: { value: '4500' } })
    fireEvent.click(screen.getAllByText('Save Basic')[0])

    await waitFor(() => {
      expect(dc.setBatteryLegacy).toHaveBeenCalledWith('dev-1', 0, 4500, 100)
    })
  })

  it('Battery Reset Coulomb calls resetCoulomb', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('batteries'))
    fireEvent.click(screen.getAllByText('Reset Coulomb')[0])

    await waitFor(() => {
      expect(dc.resetCoulomb).toHaveBeenCalledWith('dev-1', 0)
    })
  })

  it('Calibration Save calls setCalibration (4 types per channel)', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('calibration'))
    // The first row's Save button — sets the 4 calibration types for channel 0
    const saves = screen.getAllByText(/^Save$/)
    fireEvent.click(saves[0])

    await waitFor(() => {
      expect(dc.setCalibration).toHaveBeenCalledTimes(4)
      expect(dc.setCalibration).toHaveBeenNthCalledWith(1, 'dev-1', 0, 0, 0)
      expect(dc.setCalibration).toHaveBeenNthCalledWith(2, 'dev-1', 0, 1, 1)
      expect(dc.setCalibration).toHaveBeenNthCalledWith(3, 'dev-1', 0, 2, 0)
      expect(dc.setCalibration).toHaveBeenNthCalledWith(4, 'dev-1', 0, 3, 1)
    })
  })

  it('Start Baseline Calibration calls calibrateBaseline', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('calibration'))
    fireEvent.click(screen.getByText('Start Baseline Calibration'))

    await waitFor(() => {
      expect(dc.calibrateBaseline).toHaveBeenCalledWith('dev-1')
    })
  })

  it('Reboot Device calls reboot', async () => {
    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('system'))
    fireEvent.click(screen.getByText('Reboot Device'))

    await waitFor(() => {
      expect(dc.reboot).toHaveBeenCalledWith('dev-1', '')
    })
  })

  it('Factory Reset calls factoryReset', async () => {
    // The page uses window.confirm to confirm the factory reset
    vi.spyOn(window, 'confirm').mockReturnValue(true)

    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('system'))
    fireEvent.click(screen.getByText('Factory Reset'))

    await waitFor(() => {
      expect(dc.factoryReset).toHaveBeenCalledWith('dev-1', '')
    })
  })

  it('propagates errors from setWifi to the message banner', async () => {
    ;(dc.setWifi as ReturnType<typeof vi.fn>).mockRejectedValueOnce(new Error('network down'))

    await renderAndWaitForReady()
    const ssidInput = screen.getAllByPlaceholderText('WiFi name')[0] as HTMLInputElement
    const passInput = screen.getAllByPlaceholderText('Password')[0] as HTMLInputElement
    fireEvent.change(ssidInput, { target: { value: 'X' } })
    fireEvent.change(passInput, { target: { value: 'Y' } })
    fireEvent.click(screen.getByText('Save WiFi'))

    await waitFor(() => {
      expect(screen.getByText(/Error: network down/)).toBeInTheDocument()
    })
  })

  it('propagates errors from setShunt to the message banner', async () => {
    // Save Shunts issues 3 setShunt calls (one per channel). Make all
    // three reject so the error banner is what stays visible.
    ;(dc.setShunt as ReturnType<typeof vi.fn>).mockRejectedValue(new Error('queue full'))

    await renderAndWaitForReady()
    fireEvent.click(screen.getByText('sensors'))
    const input = document.getElementById('shunt-0') as HTMLInputElement
    fireEvent.change(input, { target: { value: '0.001' } })
    fireEvent.click(screen.getByText('Save Shunts'))

    await waitFor(() => {
      expect(screen.getByText(/Error: queue full/)).toBeInTheDocument()
    })
  })
})
