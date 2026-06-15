import {
  Chart as ChartJS,
  CategoryScale,
  LinearScale,
  PointElement,
  LineElement,
  BarElement,
  Title,
  Tooltip,
  Legend,
} from 'chart.js'
import { Bar } from 'react-chartjs-2'
import type { TelemetryPoint } from '../../lib/types'

ChartJS.register(CategoryScale, LinearScale, PointElement, LineElement, BarElement, Title, Tooltip, Legend)

interface DashboardChartProps {
  data: TelemetryPoint[]
  metric: string
}

export default function DashboardChart({ data, metric }: DashboardChartProps) {
  const labels = data.map((p) => new Date(p.recorded_at).toLocaleTimeString())
  const values = data.map((p) => p.payload[metric] ?? 0)

  return (
    <div className="bg-white rounded-lg border border-gray-200 p-4 shadow-sm h-80">
      <Bar
        data={{
          labels,
          datasets: [
            {
              label: metric,
              data: values,
              backgroundColor: '#f97316',
              borderRadius: 2,
            },
          ],
        }}
        options={{
          responsive: true,
          maintainAspectRatio: false,
          plugins: { legend: { display: false } },
          scales: {
            x: { grid: { display: false } },
            y: { beginAtZero: true },
          },
        }}
      />
    </div>
  )
}