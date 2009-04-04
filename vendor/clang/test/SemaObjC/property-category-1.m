// RUN: clang -fsyntax-only -verify %s

@interface Object
- (id)new;
@end

@interface ReadOnly : Object
{
  int _object;
  int _Anotherobject;
}
@property(readonly) int object;
@property(readonly) int Anotherobject;
@end

@interface ReadOnly ()
@property(readwrite) int object;
@property(readwrite, setter = myAnotherobjectSetter:) int Anotherobject;
@end

@implementation ReadOnly
@synthesize object = _object;
@synthesize  Anotherobject = _Anotherobject;
- (void) myAnotherobjectSetter : (int)val {
    _Anotherobject = val;
}
@end

int main(int argc, char **argv) {
    ReadOnly *test = [ReadOnly new];
    test.object = 12345;
    test.Anotherobject = 200;
    return test.object - 12345 + test.Anotherobject - 200;
}

///

@interface I0
@property(readonly) int p0;
@end 

@interface I0 (Cat0)
@end 

@interface I0 (Cat1)
@end 
  
@implementation I0
- (void) foo {
  self.p0 = 0; // expected-error {{assigning to property with 'readonly' attribute not allowed}}
}
@end