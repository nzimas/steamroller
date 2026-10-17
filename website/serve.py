"""Serve the SteamRoller installer on this computer (http://127.0.0.1 is a secure
context for Web MIDI). Starting it never opens MIDI or sends firmware; the
same folder can be published as-is on GitHub Pages."""
import argparse
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import webbrowser

ROOT=Path(__file__).resolve().parent


def open_browser(url):
    if sys.platform=='darwin':
        # Use Launch Services explicitly and report failures instead of silently
        # spawning an unchecked child process from a Finder-launched Terminal.
        for name,bundle in [('Google Chrome','com.google.Chrome'),('Microsoft Edge','com.microsoft.edgemac')]:
            try:
                result=subprocess.run(['/usr/bin/open','-b',bundle,url],capture_output=True,text=True,timeout=15)
                if result.returncode==0:
                    print(f'Asked macOS to open {name}. If no tab appears, paste this address into Chrome or Edge:\n{url}',flush=True)
                    return
                detail=(result.stderr or result.stdout).strip()
                print(f'macOS could not open {name}: {detail}',flush=True)
            except (OSError,subprocess.TimeoutExpired) as error:
                print(f'Browser launch failed: {error}',flush=True)
    else:
        try:
            if webbrowser.open(url):return
        except webbrowser.Error as error:
            print(f'Browser launch failed: {error}',flush=True)
    print(f'The server is running. Open Chrome or Edge and paste:\n{url}',flush=True)


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.send_header('Referrer-Policy','no-referrer')
        # as on GitHub Pages: the page's own files, plus the web font and the PayPal button
        self.send_header('Content-Security-Policy',"default-src 'self'; script-src 'self'; style-src 'self' https://fonts.googleapis.com; font-src https://fonts.gstatic.com; img-src 'self' https://www.paypalobjects.com; form-action https://www.paypal.com; connect-src 'self'; object-src 'none'; base-uri 'none'")
        super().end_headers()

    def allowed_path(self):
        path=Path(self.translate_path(self.path)).resolve()
        try:path.relative_to(ROOT)
        except ValueError:
            self.send_error(403,'Outside the installer folder');return False
        return True

    def do_GET(self):
        if self.allowed_path():super().do_GET()

    def do_HEAD(self):
        if self.allowed_path():super().do_HEAD()

    def list_directory(self,path):
        self.send_error(403,'Directory listing disabled');return None

    def guess_type(self,path):
        if path.endswith('.mjs'):return 'text/javascript'
        return super().guess_type(path)

    def log_message(self,fmt,*args):
        if args and str(args[1:2]) not in ("('200',)",):
            print(fmt%args,file=sys.stderr)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-open',action='store_true',help='Print URL without opening a browser')
    parser.add_argument('--port',type=int,default=8765)
    args=parser.parse_args()
    try:server=ThreadingHTTPServer(('127.0.0.1',args.port),partial(Handler,directory=str(ROOT)))
    except OSError:
        server=ThreadingHTTPServer(('127.0.0.1',0),partial(Handler,directory=str(ROOT)))
    url=f'http://127.0.0.1:{server.server_port}/'
    print(f'SteamRoller installer: {url}',flush=True)
    print('Press Ctrl+C to close this server.',flush=True)
    if not args.no_open:
        open_browser(url)
    try:server.serve_forever()
    except KeyboardInterrupt:pass
    finally:server.server_close()


if __name__=='__main__':main()
