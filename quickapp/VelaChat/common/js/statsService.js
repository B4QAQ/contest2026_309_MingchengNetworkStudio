import file from '@system.file'
import { loadSessions } from './chatStorage'

const STATS_FILE_PATH = 'internal://files/mail/stats.json'

const DEFAULT_STATS = {
  sessionCount: 0,
  messageCount: 0,
  tokenTotal: 0,
  activeDays: 0,
  currentStreak: 0,
  longestStreak: 0,
  peakHour: 0,
  model: 'Opus 5',
  dailyActivity: {}
}

function readJsonFile(path) {
  return new Promise((resolve) => {
    file.readText({
      uri: path,
      success: (data) => {
        try {
          resolve(JSON.parse(data.text))
        } catch (e) {
          resolve(null)
        }
      },
      fail: () => resolve(null)
    })
  })
}

function writeJsonFile(path, data) {
  return new Promise((resolve, reject) => {
    file.writeText({
      uri: path,
      text: JSON.stringify(data),
      success: () => resolve(true),
      fail: (data, code) => reject('写入失败: ' + code)
    })
  })
}

function getTodayStr() {
  const d = new Date()
  const y = d.getFullYear()
  const m = (d.getMonth() + 1).toString().padStart(2, '0')
  const day = d.getDate().toString().padStart(2, '0')
  return y + '-' + m + '-' + day
}

function getHour() {
  return new Date().getHours()
}

function countActiveDays(dailyActivity) {
  return Object.keys(dailyActivity).length
}

function countCurrentStreak(dailyActivity) {
  const today = getTodayStr()
  let streak = 0
  let d = new Date()

  for (let i = 0; i < 365; i++) {
    const key = d.getFullYear() + '-' + (d.getMonth() + 1).toString().padStart(2, '0') + '-' + d.getDate().toString().padStart(2, '0')
    if (dailyActivity[key]) {
      streak++
    } else {
      if (i === 0) {
        d.setDate(d.getDate() - 1)
        continue
      }
      break
    }
    d.setDate(d.getDate() - 1)
  }
  return streak
}

function countLongestStreak(dailyActivity) {
  const dates = Object.keys(dailyActivity).sort()
  if (dates.length === 0) return 0

  let longest = 1
  let current = 1

  for (let i = 1; i < dates.length; i++) {
    const prev = new Date(dates[i - 1])
    const curr = new Date(dates[i])
    const diff = (curr - prev) / (1000 * 60 * 60 * 24)
    if (diff === 1) {
      current++
      if (current > longest) longest = current
    } else {
      current = 1
    }
  }
  return longest
}

function findPeakHour(dailyActivity) {
  const hourCounts = {}
  for (const key in dailyActivity) {
    const activity = dailyActivity[key]
    if (activity && activity.messages) {
      // 统计每个小时的消息数
      const hour = new Date(key).getHours()
      hourCounts[hour] = (hourCounts[hour] || 0) + activity.messages
    }
  }
  let peakHour = 10 // 默认上午10点
  let maxCount = 0
  for (const h in hourCounts) {
    if (hourCounts[h] > maxCount) {
      maxCount = hourCounts[h]
      peakHour = parseInt(h)
    }
  }
  return peakHour
}

export async function getStats() {
  const saved = await readJsonFile(STATS_FILE_PATH)
  if (!saved) return Object.assign({}, DEFAULT_STATS)

  const stats = Object.assign({}, DEFAULT_STATS, saved)
  // 实时获取会话数量
  try {
    const sessions = await loadSessions()
    stats.sessionCount = sessions.length
  } catch (e) {
    stats.sessionCount = 0
  }
  stats.activeDays = countActiveDays(stats.dailyActivity)
  stats.currentStreak = countCurrentStreak(stats.dailyActivity)
  stats.longestStreak = countLongestStreak(stats.dailyActivity)
  stats.peakHour = findPeakHour(stats.dailyActivity)
  stats.model = 'mimo-v2.5-pro'
  return stats
}

export async function recordMessage(role, usage) {
  const saved = await readJsonFile(STATS_FILE_PATH)
  const stats = saved || Object.assign({}, DEFAULT_STATS)

  stats.messageCount = (stats.messageCount || 0) + 1

  const today = getTodayStr()
  if (!stats.dailyActivity[today]) {
    stats.dailyActivity[today] = { messages: 0, tokens: 0 }
  }
  stats.dailyActivity[today].messages = (stats.dailyActivity[today].messages || 0) + 1

  // 使用真实 token 数据，无则估算
  let tokens = 0
  if (usage && usage.total_tokens) {
    tokens = usage.total_tokens
  } else {
    tokens = role === 'user' ? 50 : 200
  }
  stats.tokenTotal = (stats.tokenTotal || 0) + tokens
  stats.dailyActivity[today].tokens = (stats.dailyActivity[today].tokens || 0) + tokens

  await writeJsonFile(STATS_FILE_PATH, stats)
}

export async function recordSession() {
  const saved = await readJsonFile(STATS_FILE_PATH)
  const stats = saved || Object.assign({}, DEFAULT_STATS)
  stats.sessionCount = (stats.sessionCount || 0) + 1
  await writeJsonFile(STATS_FILE_PATH, stats)
}

export async function getHeatmapData() {
  const saved = await readJsonFile(STATS_FILE_PATH)
  if (!saved || !saved.dailyActivity) return []

  const rows = []
  const today = new Date()

  // 获取今天是星期几（0=周日，1=周一...6=周六）
  const todayDay = today.getDay()

  // 生成5行（周）×7列（日）矩阵
  // 从今天往前推，周数从小到大显示
  for (let week = 4; week >= 0; week--) {
    const cells = []
    for (let day = 0; day < 7; day++) {
      // 计算日期：从今天往前推
      // week=0 是当前周，day=0 是周一，day=6 是周日
      // todayDay: 0=周日, 1=周一, ..., 6=周六
      // 转换为 0=周一, ..., 6=周日
      const todayDayMon = todayDay === 0 ? 6 : todayDay - 1
      const daysAgo = week * 7 + todayDayMon - day
      const d = new Date(today)
      d.setDate(d.getDate() - daysAgo)
      const key = d.getFullYear() + '-' + (d.getMonth() + 1).toString().padStart(2, '0') + '-' + d.getDate().toString().padStart(2, '0')
      const activity = saved.dailyActivity[key]
      let level = 0
      if (activity) {
        const msgs = activity.messages || 0
        if (msgs >= 20) level = 4
        else if (msgs >= 10) level = 3
        else if (msgs >= 5) level = 2
        else level = 1
      }
      cells.push({ date: key, level: level })
    }
    // 计算这一周对应的年周号
    const weekDate = new Date(today)
    weekDate.setDate(weekDate.getDate() - week * 7)
    const weekNum = getWeekNumber(weekDate)
    rows.push({ week: weekNum, cells: cells })
  }
  return rows
}

function getWeekNumber(d) {
  const date = new Date(d)
  date.setHours(0, 0, 0, 0)
  // 设置到本周四
  date.setDate(date.getDate() + 3 - (date.getDay() + 6) % 7)
  const week1 = new Date(date.getFullYear(), 0, 4)
  return 1 + Math.round(((date - week1) / 86400000 - 3 + (week1.getDay() + 6) % 7) / 7)
}

export function formatTokenCount(count) {
  if (count >= 1000000) {
    return (count / 1000000).toFixed(1) + 'M'
  }
  if (count >= 1000) {
    return (count / 1000).toFixed(1) + 'K'
  }
  return count.toString()
}
