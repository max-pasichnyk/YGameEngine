/****************************************************************************
** Meta object code from reading C++ file 'EditorResourceSelectionWidget.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.4.0)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "Editor/PrecompiledHeader.h"
#include "EditorResourceSelectionWidget.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'EditorResourceSelectionWidget.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.4.0. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
struct qt_meta_stringdata_EditorResourceSelectionWidget_t {
    QByteArrayData data[10];
    char stringdata[160];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_EditorResourceSelectionWidget_t, stringdata) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_EditorResourceSelectionWidget_t qt_meta_stringdata_EditorResourceSelectionWidget = {
    {
QT_MOC_LITERAL(0, 0, 29), // "EditorResourceSelectionWidget"
QT_MOC_LITERAL(1, 30, 12), // "valueChanged"
QT_MOC_LITERAL(2, 43, 0), // ""
QT_MOC_LITERAL(3, 44, 5), // "value"
QT_MOC_LITERAL(4, 50, 19), // "setResourceTypeInfo"
QT_MOC_LITERAL(5, 70, 23), // "const ResourceTypeInfo*"
QT_MOC_LITERAL(6, 94, 17), // "pResourceTypeInfo"
QT_MOC_LITERAL(7, 112, 8), // "setValue"
QT_MOC_LITERAL(8, 121, 21), // "onBrowseButtonClicked"
QT_MOC_LITERAL(9, 143, 16) // "onLineEditEdited"

    },
    "EditorResourceSelectionWidget\0"
    "valueChanged\0\0value\0setResourceTypeInfo\0"
    "const ResourceTypeInfo*\0pResourceTypeInfo\0"
    "setValue\0onBrowseButtonClicked\0"
    "onLineEditEdited"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_EditorResourceSelectionWidget[] = {

 // content:
       7,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   39,    2, 0x06 /* Public */,

 // slots: name, argc, parameters, tag, flags
       4,    1,   42,    2, 0x0a /* Public */,
       7,    1,   45,    2, 0x0a /* Public */,
       8,    0,   48,    2, 0x08 /* Private */,
       9,    1,   49,    2, 0x08 /* Private */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString,    3,

 // slots: parameters
    QMetaType::Void, 0x80000000 | 5,    6,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    3,

       0        // eod
};

void EditorResourceSelectionWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        EditorResourceSelectionWidget *_t = static_cast<EditorResourceSelectionWidget *>(_o);
        switch (_id) {
        case 0: _t->valueChanged((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 1: _t->setResourceTypeInfo((*reinterpret_cast< const ResourceTypeInfo*(*)>(_a[1]))); break;
        case 2: _t->setValue((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->onBrowseButtonClicked(); break;
        case 4: _t->onLineEditEdited((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        void **func = reinterpret_cast<void **>(_a[1]);
        {
            typedef void (EditorResourceSelectionWidget::*_t)(const QString & );
            if (*reinterpret_cast<_t *>(func) == static_cast<_t>(&EditorResourceSelectionWidget::valueChanged)) {
                *result = 0;
            }
        }
    }
}

const QMetaObject EditorResourceSelectionWidget::staticMetaObject = {
    { &QWidget::staticMetaObject, qt_meta_stringdata_EditorResourceSelectionWidget.data,
      qt_meta_data_EditorResourceSelectionWidget,  qt_static_metacall, Q_NULLPTR, Q_NULLPTR}
};


const QMetaObject *EditorResourceSelectionWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *EditorResourceSelectionWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return Q_NULLPTR;
    if (!strcmp(_clname, qt_meta_stringdata_EditorResourceSelectionWidget.stringdata))
        return static_cast<void*>(const_cast< EditorResourceSelectionWidget*>(this));
    return QWidget::qt_metacast(_clname);
}

int EditorResourceSelectionWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void EditorResourceSelectionWidget::valueChanged(const QString & _t1)
{
    void *_a[] = { Q_NULLPTR, const_cast<void*>(reinterpret_cast<const void*>(&_t1)) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}
QT_END_MOC_NAMESPACE
