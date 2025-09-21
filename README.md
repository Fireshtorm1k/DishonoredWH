# Dishonored Wallhack

## Build

```bash
git clone --recurse-submodules https://github.com/Fireshtorm1k/DishonoredWH.git
cmake -B build
cmake --build build --config Release
```

## How to use
- Inject dll in Dishonored2.exe
- Toggle menu - `Home`
- After loading on the level, press Reload Cache.
- To exclude some objects you can add substring in filter tab, after this you should trigger Reload cache
- Before loading on other level(Or if you died), you should clean the list to prevent access violation (I think i'll do it automatically in future)

## Some screenshots
<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/73b14f5a-4b31-47ed-a93e-3d24085098e7" />
<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/4881758d-f924-49a5-a417-28e9ba375928" />
<img width="2560" height="1440" alt="image" src="https://github.com/user-attachments/assets/4015d408-63d8-4ae5-84f8-06c51cacd837" />


## Contributing

If you want to make this tool better, you could do pull request.
