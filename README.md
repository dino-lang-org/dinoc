# Dino programming language – what is this?
> An experiment to create a compiler for programming language by AI

# What does it do?
> Now it can compile source code to the object files and link simple programs! Examples will be soon(ig)..

# Hello World in Dino
```dino
@include("std"); // include standard library

// declare structure describe Point
struct Point {
    public float x, y;
    
    public Point(float x, float y) {
        stdout.println("Creating point with x=%f, y=%f", x, y);
        this->x = x;
        this->y = y;
    }
    
    public ~Point() {
        stdout.println("Destroying point with x=%f, y=%f", this->x, this->y);
    }
}

#[no_mangle] // why? see the following description
int32 main() {
    Point p = Point(1.0, 2.0);
    return 0;
}
```
### Description
> Q: Why do I need to use `#[no_mangle]`?
>
> A: Now you need to use it because the compiler now generates the object files and the C/C++ compiler requires the 'main' symbol as an entry point and Dino has mangling
