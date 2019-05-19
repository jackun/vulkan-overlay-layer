A Vulkan layer to display information using an overlay.

To turn on the layer run :

VK_INSTANCE_LAYERS=VK_LAYER_NUUDEL_overlay [ENABLE_NUUDEL_LAYER=1] /path/to/my_vulkan_app

### Customize some settings :

* set overlay starting position, in pixels:
  - NUUDEL_POS=xpos,ypos
* show average cpu usage instead:
  - NUUDEL_AVGCPU=1
* amdgpu index (really hwmon though), if set shows some hwmon stats:
  - NUUDEL_AMDGPU_INDEX=0
* change text color, alpha is optional:
  - NUUDEL_RGBA=255,128,64[,255]
* unix socket path. Send text to overlay:
  - NUUDEL_SOCKET=/tmp/nuudel.socket

Socket examples:


Lines are separated by new line ('\n') and null character ('\0') clears saved lines or if line count is over 15.
 
```
echo -ne 'Test line 1\nTest line 2\nTest line 3\n\0' | socat - unix-client:/tmp/nuudel.socket
```

```
echo Test line 1 | socat - unix-client:/tmp/nuudel.socket
echo Test line 2 | socat - unix-client:/tmp/nuudel.socket
echo Test line 3 | socat - unix-client:/tmp/nuudel.socket
echo -ne '\0' | socat - unix-client:/tmp/nuudel.socket # set clear flag, new data clears old lines
```
