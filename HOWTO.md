# CMAKE

```sh
mkdir build
cd build
cmake .. -T "host=x64" -DBUILD_TESTING=OFF
cmake --build . --config Release -- -m

ctest -C Release --verbose
```
