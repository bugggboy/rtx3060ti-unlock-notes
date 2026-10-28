#!/usr/bin/env python3
"""通过 GitHub Git Data API 推送本地目录（git 协议被阻断时的替代方案）。

用法：
    set GH_TOKEN=ghp_xxx
    python push-via-api.py --repo bugggboy/rtx3060ti-unlock-notes --dir . --message "提交说明"

会以当前远端 main 为父提交，把目录内所有文件作为一个新提交推上去。
"""
import argparse, base64, json, os, sys, urllib.error, urllib.request

API = 'https://api.github.com'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--repo', required=True, help='owner/name')
    ap.add_argument('--dir', required=True, help='本地目录')
    ap.add_argument('--message', required=True)
    ap.add_argument('--branch', default='main')
    args = ap.parse_args()

    token = os.environ.get('GH_TOKEN')
    if not token:
        sys.exit('缺少环境变量 GH_TOKEN')

    def call(method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(API + path, data=data, method=method)
        req.add_header('Authorization', 'token ' + token)
        req.add_header('Accept', 'application/vnd.github+json')
        req.add_header('User-Agent', 'push-via-api')
        if data:
            req.add_header('Content-Type', 'application/json')
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                txt = r.read().decode()
                return r.status, (json.loads(txt) if txt else None)
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()[:400]

    st, ref = call('GET', f'/repos/{args.repo}/git/ref/heads/{args.branch}')
    if st != 200:
        sys.exit(f'读取分支失败 {st}: {ref}')
    head = ref['object']['sha']
    st, commit = call('GET', f'/repos/{args.repo}/git/commits/{head}')
    if st != 200:
        sys.exit(f'读取提交失败 {st}: {commit}')
    base_tree = commit['tree']['sha']

    files = []
    for root, dirs, names in os.walk(args.dir):
        dirs[:] = [d for d in dirs if d not in ('.git', 'state', '__pycache__')]
        for n in names:
            if n.endswith(('.pyc',)):
                continue
            full = os.path.join(root, n)
            rel = os.path.relpath(full, args.dir).replace('\\', '/')
            files.append((rel, full))
    files.sort()

    tree = []
    for rel, full in files:
        with open(full, 'rb') as f:
            content = base64.b64encode(f.read()).decode()
        st, blob = call('POST', f'/repos/{args.repo}/git/blobs',
                        {'content': content, 'encoding': 'base64'})
        if st not in (200, 201):
            sys.exit(f'上传 {rel} 失败 {st}: {blob}')
        tree.append({'path': rel, 'mode': '100644', 'type': 'blob', 'sha': blob['sha']})
        print(f'  blob  {rel}')

    st, new_tree = call('POST', f'/repos/{args.repo}/git/trees',
                        {'base_tree': base_tree, 'tree': tree})
    if st not in (200, 201):
        sys.exit(f'创建 tree 失败 {st}: {new_tree}')

    st, new_commit = call('POST', f'/repos/{args.repo}/git/commits',
                          {'message': args.message, 'tree': new_tree['sha'], 'parents': [head]})
    if st not in (200, 201):
        sys.exit(f'创建提交失败 {st}: {new_commit}')

    st, upd = call('PATCH', f'/repos/{args.repo}/git/refs/heads/{args.branch}',
                   {'sha': new_commit['sha']})
    if st != 200:
        sys.exit(f'更新分支失败 {st}: {upd}')

    print(f'完成: {head[:7]} -> {new_commit["sha"][:7]}  共 {len(files)} 个文件')


if __name__ == '__main__':
    main()
