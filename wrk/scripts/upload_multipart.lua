-- multipart/form-data 上传压测（与 tc-src/api/api_upload.cc 中浏览器直传分支一致）
--
-- 用法：
--   wrk -t4 -c200 -d30s --latency -s scripts/upload_multipart.lua http://127.0.0.1:8081 -- \
--     <文件路径> <用户名> <md5十六进制> [表单filename] [上传路径]
--
-- 说明：
--   - URL 只写 scheme://host:port，不要带 path（path 默认 /api/upload）
--   - md5 须与文件内容一致（shell: md5sum <file> | awk '{print $1}'）
--   - 在 init 中拼好整包请求，request() 仅返回，减轻 per-request 开销

local req

local function trim(s)
  if not s then
    return ""
  end
  return (tostring(s):gsub("^%s*(.-)%s*$", "%1"))
end

function init(args)
  local file_path = trim(args[1])
  local user = trim(args[2])
  if user == "" then
    user = "bench_wrk"
  end
  local md5hex = trim(args[3])
  local filename = args[4] and trim(args[4]) or nil
  local upload_path = trim(args[5])
  if upload_path == "" then
    upload_path = "/api/upload"
  end

  if file_path == "" then
    error("upload_multipart.lua: 请在 -- 后传入 <file_path> <user> <md5_hex> [filename] [path]")
  end
  if md5hex == "" then
    error("upload_multipart.lua: 缺少 md5（示例: md5sum 文件 | awk '{print $1}'）")
  end

  local f, err = io.open(file_path, "rb")
  if not f then
    error("upload_multipart.lua: 无法打开文件 " .. file_path .. " (" .. tostring(err) .. ")")
  end
  local file_bytes = f:read("*a")
  f:close()
  local sz = #file_bytes
  if sz == 0 then
    error("upload_multipart.lua: 文件为空 " .. file_path)
  end

  if not filename or filename == "" then
    filename = file_path:match("([^/\\]+)$") or "upload.bin"
  end

  local boundary = "--------wrkTuchuangBoundaryBm4gPh7b"
  local crlf = "\r\n"
  local dash = "--"

  local parts = {}
  local function append_field(name, value)
    parts[#parts + 1] = dash .. boundary .. crlf
    parts[#parts + 1] = 'Content-Disposition: form-data; name="' .. name .. '"' .. crlf .. crlf
    parts[#parts + 1] = value .. crlf
  end

  append_field("user", user)
  append_field("md5", md5hex)
  append_field("size", tostring(sz))

  parts[#parts + 1] = dash .. boundary .. crlf
  parts[#parts + 1] = 'Content-Disposition: form-data; name="file"; filename="' .. filename .. '"' .. crlf
  parts[#parts + 1] = "Content-Type: application/octet-stream" .. crlf .. crlf
  parts[#parts + 1] = file_bytes
  parts[#parts + 1] = crlf
  parts[#parts + 1] = dash .. boundary .. dash .. crlf

  local body = table.concat(parts)
  local headers = {}
  headers["Content-Type"] = "multipart/form-data; boundary=" .. boundary

  req = wrk.format("POST", upload_path, headers, body)
end

function request()
  return req
end

done = function(summary, latency, requests)
  local dur_s = summary.duration / 1000000.0
  if dur_s <= 0 then
    dur_s = 1e-9
  end
  local qps = summary.requests / dur_s
  local p99 = latency:percentile(99)
  io.stderr:write(string.format(
    "\n---- upload_multipart.lua ----\nrequests: %d\nduration_s: %.3f\nqps: %.2f\nlatency_p99_us: %d\nlatency_p99_ms: %.3f\nhttp_status_errors: %d\n",
    summary.requests,
    dur_s,
    qps,
    p99,
    p99 / 1000.0,
    summary.errors.status
  ))
end
