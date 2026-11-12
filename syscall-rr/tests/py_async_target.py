#!/usr/local/bin/python3
"""asyncio: one thread, one epoll loop, many things in flight.  An echo server,
four clients that each open a connection per round, timers, and a subprocess
now and then.  The order in which the loop wakes the tasks comes from epoll and
the clock, so the replay must give both back exactly."""
import asyncio, hashlib, time

rnd = open("/dev/urandom", "rb", buffering=0)

async def echo(reader, writer):
    while data := await reader.read(4096):
        writer.write(data[::-1])
        await writer.drain()
    writer.close()

async def client(cid, rnd_bytes, results):
    reader, writer = await asyncio.open_connection("127.0.0.1", 7901)
    h = hashlib.sha256()
    for k in range(3):
        msg = rnd_bytes[k * 8:(k + 1) * 8].hex().encode() * (1 + rnd_bytes[k] % 50)
        writer.write(msg)
        await writer.drain()
        got = await reader.readexactly(len(msg))
        h.update(got)
        await asyncio.sleep(rnd_bytes[24 + k] / 20000)
    port = writer.get_extra_info("sockname")[1]
    writer.close()
    await writer.wait_closed()
    results.append(f"c{cid}:{port}:{h.hexdigest()[:8]}")       # completion order matters

async def main():
    server = await asyncio.start_server(echo, "127.0.0.1", 7901)
    await asyncio.sleep(1)
    i = 0
    while True:
        results = []
        await asyncio.gather(*(client(c, rnd.read(32), results) for c in range(4)))
        sub = ""
        if i % 5 == 0:
            p = await asyncio.create_subprocess_exec("/bin/echo", f"sub{i}", stdout=asyncio.subprocess.PIPE)
            out, _ = await p.communicate()
            sub = out.decode().strip() + f":{p.returncode}"
        print(f"round {i} time {time.time():.6f} mono {asyncio.get_running_loop().time():.3f} {' '.join(results)} {sub}", flush=True)
        i += 1
        await asyncio.sleep(0.05)

asyncio.run(main())
