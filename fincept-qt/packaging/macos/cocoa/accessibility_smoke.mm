#import <AppKit/AppKit.h>
#include <QApplication>
#include <QTableWidget>
#include <QAccessible>
#include <dlfcn.h>
@interface NSObject (FinceptSmoke)
+ (id)elementWithInterface:(QAccessibleInterface *)iface;
- (NSArray *)accessibilityRows;
- (NSArray *)accessibilityChildren;
- (NSString *)accessibilityRole;
- (id)accessibilityValue;
@end
int main(int argc, char **argv) {
    if (argc > 1) QCoreApplication::setLibraryPaths({QString::fromLocal8Bit(argv[1])});
    QApplication app(argc, argv);
    Class cls = NSClassFromString(@"QMacAccessibilityElement");
    if (!cls) return 2;
    Dl_info info{};
    dladdr((void *)[cls methodForSelector:@selector(elementWithInterface:)], &info);
    fprintf(stderr, "Cocoa plugin: %s\n", info.dli_fname);
    for (int cycle = 0; cycle < 20; ++cycle) {
        @autoreleasepool {
            QTableWidget table(8, 5);
            for (int row = 0; row < 8; ++row)
                for (int col = 0; col < 5; ++col)
                    table.setItem(row, col, new QTableWidgetItem("quote"));
            table.show();
            app.processEvents();
            id element = [cls elementWithInterface:QAccessible::queryAccessibleInterface(&table)];
            int cells = 0;
            for (id row in [element accessibilityRows]) {
                for (id cell in [row accessibilityChildren]) {
                    [cell accessibilityValue];
                    ++cells;
                }
            }
            if (cells != 40) {
                fprintf(stderr, "Expected 40 accessible cells, got %d\n", cells);
                return 3;
            }
            table.setRowCount(0);
            app.processEvents();
        }
    }
    fprintf(stderr, "20 accessibility create/read/reset/destroy cycles passed\n");
}
