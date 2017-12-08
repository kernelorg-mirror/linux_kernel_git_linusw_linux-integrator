=============================
Device Driver Design Patterns
=============================

This document describes a few common design patterns found in device drivers.
It is likely that subsystem maintainers will ask driver developers to
conform to these design patterns.

1. API Design
2. State Container
3. container_of()


1. API Design
~~~~~~~~~~~~~

In 2008, long-term kernel contributor Rusty Russell came up with a few simple
looking rules regarding API design. The rules are subjective to a varying
degree and some may not be attainable under all circumstances. Reaching
level 5 in C API's should in practice always be possible. The more general
and widespread the use of the API is, the more important it is for it to
conform to the highest possible level of API quality.

10. It's impossible to get wrong.
9.  The compiler/linker won't let you get it wrong.
8.  The compiler will warn if you get it wrong.
7.  The obvious use is (probably) the correct one.
6.  The name tells you how to use it.
5.  Do it right or it will always break at runtime.
4.  Follow common convention and you'll get it right.
3.  Read the documentation and you'll get it right.
2.  Read the implementation and you'll get it right.
1.  Read the correct mailing list thread and you'll get it right.

-1. Read the mailing list thread and you'll get it wrong.
-2. Read the implementation and you'll get it wrong.
-3. Read the documentation and you'll get it wrong.
-4. Follow common convention and you'll get it wrong.
-5. Do it right and it will sometimes break at runtime.
-6. The name tells you how not to use it.
-7. The obvious use is wrong.
-8. The compiler will warn if you get it right.
-9. The compiler/linker won't let you get it right.
-10. It's impossible to get right.


2. State Container
~~~~~~~~~~~~~~~~~~

While the kernel contains a few device drivers that assume that they will
only be probed() once on a certain system (singletons), it is custom to assume
that the device the driver binds to will appear in several instances. This
means that the probe() function and all callbacks need to be reentrant.

The most common way to achieve this is to use the state container design
pattern. It usually has this form::

  struct foo {
      spinlock_t lock; /* Example member */
      (...)
  };

  static int foo_probe(...)
  {
      struct foo *foo;

      foo = devm_kzalloc(dev, sizeof(*foo), GFP_KERNEL);
      if (!foo)
          return -ENOMEM;
      spin_lock_init(&foo->lock);
      (...)
  }

This will create an instance of struct foo in memory every time probe() is
called. This is our state container for this instance of the device driver.
Of course it is then necessary to always pass this instance of the
state around to all functions that need access to the state and its members.

For example, if the driver is registering an interrupt handler, you would
pass around a pointer to struct foo like this::

  static irqreturn_t foo_handler(int irq, void *arg)
  {
      struct foo *foo = arg;
      (...)
  }

  static int foo_probe(...)
  {
      struct foo *foo;

      (...)
      ret = request_irq(irq, foo_handler, 0, "foo", foo);
  }

This way you always get a pointer back to the correct instance of foo in
your interrupt handler.


3. container_of()
~~~~~~~~~~~~~~~~~

Continuing on the above example we add an offloaded work::

  struct foo {
      spinlock_t lock;
      struct workqueue_struct *wq;
      struct work_struct offload;
      (...)
  };

  static void foo_work(struct work_struct *work)
  {
      struct foo *foo = container_of(work, struct foo, offload);

      (...)
  }

  static irqreturn_t foo_handler(int irq, void *arg)
  {
      struct foo *foo = arg;

      queue_work(foo->wq, &foo->offload);
      (...)
  }

  static int foo_probe(...)
  {
      struct foo *foo;

      foo->wq = create_singlethread_workqueue("foo-wq");
      INIT_WORK(&foo->offload, foo_work);
      (...)
  }

The design pattern is the same for an hrtimer or something similar that will
return a single argument which is a pointer to a struct member in the
callback.

container_of() is a macro defined in <linux/kernel.h>

What container_of() does is to obtain a pointer to the containing struct from
a pointer to a member by a simple subtraction using the offsetof() macro from
standard C, which allows something similar to object oriented behaviours.
Notice that the contained member must not be a pointer, but an actual member
for this to work.

We can see here that we avoid having global pointers to our struct foo *
instance this way, while still keeping the number of parameters passed to the
work function to a single pointer.
