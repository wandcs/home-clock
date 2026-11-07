"""Generate a build-local single-file fix; never edit the shared SDK."""
import argparse, hashlib, json
from pathlib import Path
LOCKED_SHA256='ee7ddb63bb9328bb9aa44f8889081c0e5316f593c2466cb6da441f30b8989c48'
LOCKED_COMMIT='fff9895c82d744c7237be8847347bdd1b07c6643'
INIT_SETS="""        if (cfg && cfg->non_block) {
            FD_ZERO(&tls->rset);
            FD_SET(tls->sockfd, &tls->rset);
            tls->wset = tls->rset;
        }
        tls->conn_state = ESP_TLS_CONNECTING;"""
WAIT_OLD="""            if (select(tls->sockfd + 1, &tls->rset, &tls->wset, NULL,
                       cfg->timeout_ms>0 ? &tv : NULL) == 0) {"""
WAIT_NEW="""            FD_ZERO(&tls->rset);
            FD_SET(tls->sockfd, &tls->rset);
            tls->wset = tls->rset;
            int ret = select(tls->sockfd + 1, &tls->rset, &tls->wset, NULL,
                             cfg->timeout_ms>0 ? &tv : NULL);
            if (ret < 0) {
                int error = errno;
                ESP_LOGD(TAG, "Non blocking connect select failed");
                ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_SYSTEM, error);
                ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_ESP, ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST);
                tls->conn_state = ESP_TLS_FAIL;
                return -1;
            }
            if (ret == 0) {"""
ERROR_OLD="""                    ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_ESP, ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED);
                    tls->conn_state = ESP_TLS_FAIL;
                    return -1;
                }
            }
        }
        /* By now, the connection has been established */"""
ERROR_NEW="""                    ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_ESP, ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED);
                    tls->conn_state = ESP_TLS_FAIL;
                    return -1;
                }
                if (error != 0) {
                    ESP_LOGD(TAG, "Non blocking connect returned a socket error");
                    ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_SYSTEM, error);
                    ESP_INT_EVENT_TRACKER_CAPTURE(tls->error_handle, ESP_TLS_ERR_TYPE_ESP, ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST);
                    tls->conn_state = ESP_TLS_FAIL;
                    return -1;
                }
            }
        }
        /* By now, the connection has been established */"""
def sha(data): return hashlib.sha256(data).hexdigest()
def generate(source):
    if sha(source)!=LOCKED_SHA256:
        raise ValueError('esp_tls.c differs from reviewed IDF6.1 input; review source before changing this patch')
    text=source.decode('utf-8')
    for old,new in [(INIT_SETS,'        tls->conn_state = ESP_TLS_CONNECTING;'),(WAIT_OLD,WAIT_NEW),(ERROR_OLD,ERROR_NEW)]:
        if text.count(old)!=1: raise ValueError('TLS patch context is not unique')
        text=text.replace(old,new,1)
    return text.encode('utf-8')
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--source',type=Path,required=True);ap.add_argument('--output',type=Path,required=True);ap.add_argument('--manifest',type=Path);a=ap.parse_args()
    if a.source.resolve()==a.output.resolve(): raise SystemExit('Refusing to overwrite SDK source')
    source=a.source.read_bytes()
    try: output=generate(source)
    except ValueError as error: raise SystemExit(str(error))
    a.output.parent.mkdir(parents=True,exist_ok=True)
    if not a.output.exists() or a.output.read_bytes()!=output:a.output.write_bytes(output)
    manifest={'idf_commit':LOCKED_COMMIT,'input_sha256':sha(source),'output_sha256':sha(output),'generator_sha256':sha(Path(__file__).read_bytes())}
    if a.manifest:
        a.manifest.parent.mkdir(parents=True,exist_ok=True)
        a.manifest.write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8',newline='\n')
    print(json.dumps(manifest))
if __name__=='__main__':main()
