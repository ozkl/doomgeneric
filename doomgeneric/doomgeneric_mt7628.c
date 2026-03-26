#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "doomgeneric.h"
#include "mongoose.h"

#define WIDTH  DOOMGENERIC_RESX
#define HEIGHT DOOMGENERIC_RESY
#define BPP 4   // bytes per pixel

#define KEY_QUEUE_SIZE 16

static uint8_t fb1[WIDTH * HEIGHT * BPP];
static uint8_t fb2[WIDTH * HEIGHT * BPP];
static uint8_t *front = fb1;
static uint8_t *back  = fb2;

extern pixel_t* DG_ScreenBuffer;

volatile int frame_ready = 0;

static struct
{
    uint8_t key;
    int pressed;
} key_queue[KEY_QUEUE_SIZE];
static int key_head = 0, key_tail = 0;

static struct mg_mgr mgr;
static struct mg_connection *client = NULL;

void doomgeneric_ProcessKey(uint8_t key, int pressed)
{
    int next = (key_head + 1) % KEY_QUEUE_SIZE;
    if (next != key_tail)
    {
        key_queue[key_head].key = key;
        key_queue[key_head].pressed = pressed;
        key_head = next;
    }
}

// Websocket handler
static void fn(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;

        if (mg_match(hm->uri, mg_str("/ws"), NULL))
        {
            mg_ws_upgrade(c, hm, NULL);
        }
        else
        {
            mg_http_reply(c, 200, "Content-Type: text/html\r\n",
                "<html>"
                "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"

                "<body style='background:#000;color:#0f0;margin:0;"
                "display:flex;flex-direction:column;align-items:center;"
                "font-family:monospace;touch-action:none;user-select:none'>"

                "<canvas id='c' style='width:100vw;max-width:640px;"
                "aspect-ratio:1.6;image-rendering:pixelated;"
                "border-bottom:1px solid #333'></canvas>"

                "<div style='display:flex;justify-content:space-between;"
                "width:100%;max-width:640px;padding:10px;box-sizing:border-box'>"

                    "<div style='display:grid;grid-template-columns:1fr;gap:5px'>"
                        "<button style='padding:25px' data-k='173'>UP</button>"
                        "<button style='padding:25px' data-k='175'>DN</button>"
                    "</div>"

                    "<div style='display:grid;grid-template-columns:1fr;gap:5px'>"
                        "<button style='padding:15px' data-k='163'>FIRE</button>"
                        "<button style='padding:15px' data-k='162'>USE</button>"
                        "<button style='padding:10px' data-k='9'>MAP</button>"
                    "</div>"

                    "<div style='display:grid;grid-template-columns:1fr;gap:5px'>"
                        "<button style='padding:25px' data-k='172'>LT</button>"
                        "<button style='padding:25px' data-k='174'>RT</button>"
                    "</div>"

                "</div>"

                "<script>"

                "var c=document.getElementById('c'),"
                "ctx=c.getContext('2d',{alpha:false}),"
                "i=ctx.createImageData(320,200),"
                "w=new WebSocket('ws://'+location.host+'/ws');"

                "c.width=320;c.height=200;"
                "w.binaryType='arraybuffer';"

                "w.onmessage=(e)=>{"
                    "var d=new Uint8Array(e.data),p=i.data;"
                    "for(var j=0;j<d.length;j+=4){"
                        "p[j]=d[j+2];"
                        "p[j+1]=d[j+1];"
                        "p[j+2]=d[j];"
                        "p[j+3]=255;"
                    "}"
                    "ctx.putImageData(i,0,0);"
                "};"

                "var k={38:173,40:175,37:172,39:174,87:173,83:175,65:172,68:174,"
                "81:160,82:161,69:162,32:163,27:27,13:13,9:9};"

                "var s=(id,st)=>{"
                    "if(w.readyState==1)"
                        "w.send(new Uint8Array([st,id]));"
                "};"

                "window.onkeydown=(e)=>{"
                    "if(k[e.keyCode]){"
                        "e.preventDefault();"
                        "s(k[e.keyCode],1);"
                    "}"
                "};"

                "window.onkeyup=(e)=>{"
                    "if(k[e.keyCode]){"
                        "e.preventDefault();"
                        "s(k[e.keyCode],0);"
                    "}"
                "};"

                "document.querySelectorAll('button').forEach(b=>{"
                    "var kid=parseInt(b.dataset.k);"
                    "b.style.background='#222';"
                    "b.style.color='#0f0';"
                    "b.style.border='1px solid #444';"
                    "b.style.fontSize='10px';"

                    "b.onpointerdown=(e)=>{"
                        "e.preventDefault();"
                        "b.style.background='#444';"
                        "s(kid,1);"
                    "};"

                    "b.onpointerup=(e)=>{"
                        "e.preventDefault();"
                        "b.style.background='#222';"
                        "s(kid,0);"
                    "};"

                    "b.onpointercancel=(e)=>{"
                        "e.preventDefault();"
                        "b.style.background='#222';"
                        "s(kid,0);"
                    "};"
                "});"

                "</script>"
                "</body></html>"
            );
        }
    }
    else if (ev == MG_EV_WS_MSG)
    {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;

        if (wm->data.len == 2)
        {
            const uint8_t *d = (const uint8_t *) wm->data.buf;
            doomgeneric_ProcessKey(d[1], d[0]);
        }
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        client = c;
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (c == client)
        {
            client = NULL;
        }
    }
}

static void ws_send_frame(void)
{
    if (!client || !frame_ready || client->send.len > (WIDTH * HEIGHT * BPP)) return;

    frame_ready = 0;

    mg_ws_send(client, front, WIDTH * HEIGHT * BPP, WEBSOCKET_OP_BINARY);
}

// ============================
// DOOM callbacks
// ============================
void DG_Init(void)
{
    // do nothing
    printf("DG_Init executed...\n");
}

void DG_SetWindowTitle(const char *title)
{
    // ignore in headless mode
}

void DG_DrawFrame(void)
{
    if (!client) return;

    memcpy(back, DG_ScreenBuffer, WIDTH * HEIGHT * BPP);

    // swap buffers
    uint8_t *tmp = front;
    front = back;
    back = tmp;

    frame_ready = 1;
    ws_send_frame();
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (key_tail == key_head) return 0;

    *pressed = key_queue[key_tail].pressed;
    *key = key_queue[key_tail].key;
    key_tail = (key_tail + 1) % KEY_QUEUE_SIZE;

    return 1;
}

// ============================
// entry point
// ============================

extern void doomgeneric_Tick(void);

int main(int argc, char **argv)
{
    printf("Starting Doom (WS streaming)...\n");

    // websocket init
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, "http://0.0.0.0:8000", fn, NULL);

    doomgeneric_Create(argc, argv);

    while (1)
    {
        mg_mgr_poll(&mgr, 0);

        doomgeneric_Tick();

        DG_SleepMs(1);
    }

    return 0;
}
