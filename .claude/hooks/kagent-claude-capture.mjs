#!/usr/bin/env node
/**
 * KAgent capture hook — Claude Code PostToolUse → .kagent/
 *
 * 适配 Claude Code for VS Code / CLI 的 PostToolUse 钩子。
 * stdin 接收 JSON：{ tool_name, tool_input: { file_path, ... }, session_id, cwd, ... }
 */
import path from "node:path";
import { recordFileChange, countFileLines } from "./kagent-record.mjs";

async function readStdin() {
  const chunks = [];
  for await (const chunk of process.stdin) {
    chunks.push(chunk);
  }
  const raw = Buffer.concat(chunks).toString("utf8");
  if (!raw.trim()) return null;
  return JSON.parse(raw);
}

function toRelative(filePath, root) {
  const rel = path.relative(root, filePath);
  return rel.split(path.sep).join("/");
}

readStdin()
  .then((payload) => {
    // ── 基础校验 ────────────────────────────────────────────────
    const toolName = payload?.tool_name;
    const toolInput = payload?.tool_input;

    if (!toolInput?.file_path) {
      process.exit(0);
      return;
    }

    // 只处理文件写入/编辑类工具
    const FILE_TOOLS = new Set(["Write", "Edit", "MultiEdit", "NotebookEdit"]);
    if (!FILE_TOOLS.has(toolName)) {
      process.exit(0);
    }

    // ── 解析路径 ────────────────────────────────────────────────
    const filePath = path.resolve(toolInput.file_path);

    // 优先用 cwd（Claude Code 的工作目录 = 项目根）
    const workspaceRoot =
      payload.cwd ||
      process.env.CLAUDE_PROJECT_DIR ||
      path.dirname(filePath);

    const relativeFile = toRelative(filePath, workspaceRoot);
    if (relativeFile.startsWith("..")) {
      // 文件不在项目内，跳过
      process.exit(0);
      return;
    }

    // ── 构建 edits ──────────────────────────────────────────────
    let edits;
    let oldText;

    if (toolName === "Write") {
      // Write: 新建或覆写文件
      // tool_input.content 是新内容；旧内容从 tool_response?.originalFile 获取
      oldText = payload.tool_response?.originalFile ?? "";
    } else if (toolName === "Edit") {
      // Edit: 搜索替换，有明确的 old_string / new_string
      if (
        toolInput.old_string !== undefined &&
        toolInput.new_string !== undefined
      ) {
        edits = [
          {
            old_string: toolInput.old_string,
            new_string: toolInput.new_string,
          },
        ];
      }
    } else if (toolName === "MultiEdit") {
      // MultiEdit: 可能有批量 edits 数组
      if (toolInput.edits && Array.isArray(toolInput.edits)) {
        edits = toolInput.edits.map((e) => ({
          old_string: e.old_string ?? "",
          new_string: e.new_string ?? "",
        }));
      }
    } else if (toolName === "NotebookEdit") {
      // NotebookEdit: 单元格替换
      if (
        toolInput.old_string !== undefined &&
        toolInput.new_string !== undefined
      ) {
        edits = [
          {
            old_string: toolInput.old_string,
            new_string: toolInput.new_string,
          },
        ];
      }
    }

    // ── 记录 ────────────────────────────────────────────────────
    const linesAfter = countFileLines(filePath);

    recordFileChange({
      workspaceRoot,
      relativeFile,
      linesAfter,
      edits,
      oldText,
      source: payload.hook_event_name ?? "PostToolUse",
      actor: "agent",
      conversation_id: payload.session_id ?? null,
      generation_id: null,
      editor: "claude-code",
    });

    process.exit(0);
  })
  .catch((err) => {
    console.error("[kagent-claude-capture]", err.message);
    process.exit(0);
  });
