# CMAKE

```sh
mkdir build
cd build
cmake .. -T "host=x64"

cmake --build .
```


# CTEST
```
cmake --build . --target check
```
or
```
ctest --verbose
```

## CTEST with release configurtion
```
cmake --build .  --config Release --target check
```
or
```
ctest -C Release --verbose
```
