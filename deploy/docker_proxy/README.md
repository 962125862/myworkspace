# Docker Proxy Toggle (systemd)

Goal: make Docker daemon proxy opt-in, so you can do:

- `sudo systemctl start docker_proxy` -> docker uses proxy
- `sudo systemctl stop docker_proxy`  -> docker stops using proxy

This uses a systemd drop-in file:

- `/etc/systemd/system/docker.service.d/proxy.conf`

and restarts `docker.service` when toggled.

## 1) Configure proxy values (optional)

Create `/etc/default/docker_proxy`:

```bash
sudo tee /etc/default/docker_proxy >/dev/null <<'EOF'
HTTP_PROXY=http://127.0.0.1:1095
HTTPS_PROXY=http://127.0.0.1:1095
NO_PROXY=localhost,127.0.0.1,::1,192.168.0.0/16,10.0.0.0/8
EOF
```

## 2) Install the unit

```bash
sudo cp /home/gejun/work/my_ml_work/deploy/docker_proxy/docker_proxy.service /etc/systemd/system/docker_proxy.service
sudo chmod +x /home/gejun/work/my_ml_work/deploy/docker_proxy/docker_proxy.sh
sudo systemctl daemon-reload
```

Enable on boot (optional):

```bash
sudo systemctl enable docker_proxy
```

## 3) Toggle

Enable proxy:

```bash
sudo systemctl start docker_proxy
```

Disable proxy:

```bash
sudo systemctl stop docker_proxy
```

Check status:

```bash
sudo systemctl status docker_proxy --no-pager -l
sudo /home/gejun/work/my_ml_work/deploy/docker_proxy/docker_proxy.sh status
```

Notes

- This restarts docker, which will interrupt running containers.
- This config is for the docker daemon. If you need proxies inside running containers, configure them separately.

