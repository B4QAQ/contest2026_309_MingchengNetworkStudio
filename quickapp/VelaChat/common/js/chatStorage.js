import file from '@system.file'

const BASE_PATH = 'internal://files/mail'
const SESSIONS_FILE = BASE_PATH + '/sessions.json'
const MAX_MESSAGES = 50

// ==================== 会话元数据 ====================

/**
 * 创建会话对象
 */
export function createSession(title) {
  return {
    id: 'sess_' + Date.now() + '_' + Math.random().toString(36).slice(2, 6),
    title: title || '新会话',
    createdAt: Date.now(),
    updatedAt: Date.now()
  }
}

function readJson(path) {
  return new Promise((resolve) => {
    file.readText({
      uri: path,
      success: (data) => {
        try { resolve(JSON.parse(data.text)) } catch (e) { resolve(null) }
      },
      fail: () => resolve(null)
    })
  })
}

function writeJson(path, data) {
  return new Promise((resolve, reject) => {
    file.writeText({
      uri: path,
      text: JSON.stringify(data),
      success: () => resolve(true),
      fail: (d, code) => reject('写入失败: ' + code)
    })
  })
}

function deleteFile(path) {
  return new Promise((resolve) => {
    file.delete({ uri: path, success: () => resolve(true), fail: () => resolve(false) })
  })
}

// ==================== 会话列表 ====================

export async function loadSessions() {
  const data = await readJson(SESSIONS_FILE)
  return Array.isArray(data) ? data : []
}

export async function saveSessions(sessions) {
  await writeJson(SESSIONS_FILE, sessions)
}

export async function createNewSession(title) {
  const sessions = await loadSessions()
  const session = createSession(title)
  sessions.unshift(session)
  await saveSessions(sessions)
  return session
}

export async function renameSession(sessionId, newTitle) {
  const sessions = await loadSessions()
  const idx = sessions.findIndex(s => s.id === sessionId)
  if (idx !== -1) {
    sessions[idx].title = newTitle
    sessions[idx].updatedAt = Date.now()
    await saveSessions(sessions)
  }
}

export async function deleteSession(sessionId) {
  const sessions = await loadSessions()
  const filtered = sessions.filter(s => s.id !== sessionId)
  await saveSessions(filtered)
  await deleteFile(BASE_PATH + '/' + sessionId + '.json')
}

// ==================== 会话消息 ====================

export async function loadMessages(sessionId) {
  if (!sessionId) return []
  const data = await readJson(BASE_PATH + '/' + sessionId + '.json')
  return Array.isArray(data) ? data : []
}

export async function saveMessages(sessionId, messages) {
  if (!sessionId) return
  const toSave = messages.slice(-MAX_MESSAGES)
  await writeJson(BASE_PATH + '/' + sessionId + '.json', toSave)
  // 更新会话的 updatedAt
  const sessions = await loadSessions()
  const idx = sessions.findIndex(s => s.id === sessionId)
  if (idx !== -1) {
    sessions[idx].updatedAt = Date.now()
    await saveSessions(sessions)
  }
}
