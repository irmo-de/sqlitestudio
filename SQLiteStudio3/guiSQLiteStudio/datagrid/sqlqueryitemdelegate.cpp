#include "sqlqueryitemdelegate.h"
#include "sqlquerymodel.h"
#include "sqlqueryitem.h"
#include "common/unused.h"
#include "services/notifymanager.h"
#include "sqlqueryview.h"
#include "uiconfig.h"
#include "common/utils_sql.h"
#include "schemaresolver.h"
#include <QHeaderView>
#include <QPainter>
#include <QEvent>
#include <QLineEdit>
#include <QDebug>
#include <QComboBox>
#include <QApplication>
#include <QVBoxLayout>
#include <QResizeEvent>
#include <QScrollBar>
#include <limits>
#include <QToolTip>
#include <QTextLayout>
#include <QtMath>
#include <QScreen>

bool SqlQueryItemDelegate::warnedAboutHugeContents = false;

SqlQueryItemDelegate::SqlQueryItemDelegate(QObject *parent) :
    QStyledItemDelegate(parent)
{
}

void SqlQueryItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyledItemDelegate::paint(painter, option, index);
    SqlQueryItem* item = getItem(index);

    if (item->isUncommitted())
    {
        painter->setPen(item->isCommittingError() ? QColor(Qt::red) : QColor(Qt::blue));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(option.rect.x(), option.rect.y(), option.rect.width()-1, option.rect.height()-1);
    }
}

QWidget* SqlQueryItemDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    UNUSED(option);
    if (!index.isValid())
        return nullptr;

    const SqlQueryModel* model = dynamic_cast<const SqlQueryModel*>(index.model());
    SqlQueryItem* item = model->itemFromIndex(index);

    if (item->isDeletedRow())
    {
        notifyWarn(tr("Cannot edit this cell. Details: %1").arg(tr("The row is marked for deletion.")));
        return nullptr;
    }

    if (!item->getColumn()->canEdit())
    {
        notifyWarn(tr("Cannot edit this cell. Details: %1").arg(item->getColumn()->getEditionForbiddenReason()));
        return nullptr;
    }

    if (model->isStructureOutOfDate())
    {
        notifyWarn(tr("Cannot edit this cell. Details: %1").arg(tr("Structure of this table has changed since last data was loaded. Reload the data to proceed.")));
        return nullptr;
    }

    if (!item->getColumn()->getFkConstraints().isEmpty())
        return getFkEditor(item, parent, model);

    return getEditor(item->getValue().userType(), parent);
}

QString SqlQueryItemDelegate::displayText(const QVariant& value, const QLocale& locale) const
{
    UNUSED(locale);

    if (value.type() == QVariant::Double)
        return doubleToString(value);

    return QStyledItemDelegate::displayText(value, locale);
}

void SqlQueryItemDelegate::setEditorData(QWidget* editor, const QModelIndex& index) const
{
    // No need to check or load full data, it is already preloaded if necessary in createEditor().
    QComboBox* cb = dynamic_cast<QComboBox*>(editor);
    QLineEdit* le = dynamic_cast<QLineEdit*>(editor);
    if (cb) {
        setEditorDataForFk(cb, index);
    } else if (le) {
        setEditorDataForLineEdit(le, index);
    } else {
        QStyledItemDelegate::setEditorData(editor, index);
    }
}

void SqlQueryItemDelegate::setEditorDataForFk(QComboBox* cb, const QModelIndex& index) const
{
    UNUSED(cb);
    UNUSED(index);
    // There used to be code here, but it's empty now.
    // All necessary data population happens in the fkDataReady().
    // Keeping this method just for this comment and for consistency across different kind of cell editors
    // (i.e. each editor has method to copy value from model to editor and another to copy from editor to model).
}

void SqlQueryItemDelegate::setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const
{
    QComboBox* cb = dynamic_cast<QComboBox*>(editor);
    QLineEdit* le = dynamic_cast<QLineEdit*>(editor);
    if (cb) {
        setModelDataForFk(cb, model, index);
    } else if (le) {
        setModelDataForLineEdit(le, model, index);
    } else {
        QStyledItemDelegate::setModelData(editor, model, index);
    }

    SqlQueryModel* queryModel = const_cast<SqlQueryModel*>(dynamic_cast<const SqlQueryModel*>(index.model()));
    queryModel->notifyItemEditionEnded(index);
}

void SqlQueryItemDelegate::setModelDataForFk(QComboBox* cb, QAbstractItemModel* model, const QModelIndex& index) const
{
    SqlQueryModel* cbModel = dynamic_cast<SqlQueryModel*>(cb->model());
    if (cbModel->isExecutionInProgress() || !cbModel->isAllDataLoaded())
        return;

    int idx = cb->currentIndex();
    QModelIndex cbCol0Index = cbModel->index(idx, 0);
    QString cbText = cb->currentText();
    bool customValue = false;

    if (cb->currentIndex() > -1 && !cbModel->itemFromIndex(cbCol0Index))
    {
        // Inserted QStandardItem by QComboBox, meaning custom value (out of dropdown model)
        // With Qt 5.15 (maybe earlier) QComboBox started inserting QStandardItems and setting them as currentIndex.
        // Here we're extracting this inserted value and remembering this is the custom value.
        cbText = cbModel->data(cbCol0Index).toString();
        customValue = true;
    }

    // Regardless if its preselected value or custom value, we need to honor empty=null setting
    if (CFG_UI.General.KeepNullWhenEmptyValue.get() && model->data(index, Qt::EditRole).isNull() && cbText.isEmpty())
        return;

    SqlQueryModel* dataModel = dynamic_cast<SqlQueryModel*>(model);
    SqlQueryItem* theItem = dataModel->itemFromIndex(index);

    // If the item of the main data model cannot be found, we use plain QStandardItem method to set item's value.
    // It's a safety circuit breaker. Shouldn't happend and if does so, it prints Critical debug log.
    if (!theItem)
    {
        qCritical() << "Confirmed FK edition, but there is no SqlQueryItem for which this was triggered!" << index;
        model->setData(index, cbText, Qt::EditRole);
        return;
    }

    // Out of index? So it's custom value. Set it and it's done.
    // If we deal with custom value inserted as item, we also just set it and that's it.
    if (idx < 0 || idx >= cbModel->rowCount() || customValue)
    {
        theItem->setValue(cbText);
        return;
    }

    // Otherwise we will have at least 2 columns. 1st column is hidden and is meta-column holding 1/0 (1 for value matching current cell value)
    QList<SqlQueryItem *> row = cbModel->getRow(idx);
    if (row.size() < 2 || !row[1])
    {
        // This happens when inexisting value is confirmed with "Enter" key,
        // and rowCount() is apparently incremented, but items not yet.
        // Very likely this was addressed in recent Qt versions (5.15 or a bit earlier)
        // which resulted in value insertion and the "customValue" flag above in this method.
        qCritical() << "Confirmed FK edition, but there is no combo item in the row for index" << idx << ", the item is" << theItem << ", CB row count is" << cbModel->rowCount();
        theItem->setValue(cbText);
        return;
    }

    QVariant comboData = row[1]->getValue();
    theItem->setValue(comboData);
}

void SqlQueryItemDelegate::setModelDataForLineEdit(QLineEdit* editor, QAbstractItemModel* model, const QModelIndex& index) const
{
    QString value = editor->text();

    if (CFG_UI.General.KeepNullWhenEmptyValue.get() && model->data(index, Qt::EditRole).isNull() && value.isEmpty())
        return;

    const SqlQueryModel* queryModel = dynamic_cast<const SqlQueryModel*>(model);
    SqlQueryItem* item = queryModel->itemFromIndex(index);

    if (item->getColumn()->dataType.isNumeric())
    {
        bool ok;
        QVariant variant = value.toLongLong(&ok);
        if (ok)
        {
            model->setData(index, variant, Qt::EditRole);
            return;
        }

        variant = value.toDouble(&ok);
        if (ok)
        {
            model->setData(index, variant, Qt::EditRole);
            return;
        }
    }
    model->setData(index, value, Qt::EditRole);
}

void SqlQueryItemDelegate::setEditorDataForLineEdit(QLineEdit* le, const QModelIndex& index) const
{
    QVariant value = index.data(Qt::EditRole);
    if (value.userType() == QVariant::Double)
    {
        le->setText(doubleToString(value));
        return;
    }

    QString str = value.toString();
    if (str.size() > HUGE_CONTENTS_WARNING_LIMIT && !warnedAboutHugeContents)
    {
        NOTIFY_MANAGER->warn(tr("Editing a huge contents in an inline cell editor is not a good idea. It can become slow and inconvenient. It's better to edit such big contents in a Form View, or in popup editor (available under rick-click menu)."));
        warnedAboutHugeContents = true;
    }

    le->setText(str);
}

SqlQueryItem* SqlQueryItemDelegate::getItem(const QModelIndex &index) const
{
    const SqlQueryModel* queryModel = dynamic_cast<const SqlQueryModel*>(index.model());
    return queryModel->itemFromIndex(index);
}

QWidget* SqlQueryItemDelegate::getEditor(int type, QWidget* parent) const
{
    UNUSED(type);
    QLineEdit *editor = new QLineEdit(parent);
    editor->setMaxLength(std::numeric_limits<int>::max());
    editor->setFrame(editor->style()->styleHint(QStyle::SH_ItemView_DrawDelegateFrame, 0, editor));
    return editor;
}

QString SqlQueryItemDelegate::getSqlForFkEditor(SqlQueryItem* item) const
{
    static_qstring(sql, "SELECT %4, %1 FROM %2%3");
    static_qstring(currValueTpl, "(%1 == %2) AS %3");
    static_qstring(currNullValueTpl, "(%1 IS NULL) AS %2");
    static_qstring(dbColTpl, "%1 AS %2");
    static_qstring(conditionTpl, "%1.%2 = %3.%4");
    static_qstring(conditionPrefixTpl, " WHERE %1");

    QStringList selectedCols;
    QStringList fkConditionTables;
    QStringList fkConditionCols;
    QStringList srcCols;
    Db* db = item->getModel()->getDb();
    SchemaResolver resolver(db);

    QList<SqlQueryModelColumn::ConstraintFk*> fkList = item->getColumn()->getFkConstraints();
    int i = 0;
    QString src;
    QString fullSrcCol;
    QString col;
    QString firstSrcCol;
    QStringList usedNames;
    for (SqlQueryModelColumn::ConstraintFk* fk : fkList)
    {
        col = wrapObjIfNeeded(fk->foreignColumn);
        src = wrapObjIfNeeded(fk->foreignTable);
        if (i == 0)
        {
            firstSrcCol = col;
            fullSrcCol = src + "." + col;
            selectedCols << dbColTpl.arg(fullSrcCol, wrapObjIfNeeded(item->getColumn()->column));
        }

        if (fkConditionTables.contains(src, Qt::CaseInsensitive))
            continue;

        srcCols = resolver.getTableColumns(src);
        for (const QString& srcCol : srcCols)
        {
            if (fk->foreignColumn.compare(srcCol, Qt::CaseInsensitive) == 0)
                continue; // Exclude matching column. We don't want the same column several times.

            fullSrcCol = src + "." + wrapObjIfNeeded(srcCol);
            selectedCols << fullSrcCol;
            usedNames << srcCol;
        }

        fkConditionCols << col;
        fkConditionTables << src;

        i++;
    }

    QStringList conditions;
    QString firstSrc = fkConditionTables.first();
    QString firstCol = fkConditionCols.first();
    for (i = 1; i < fkConditionTables.size(); i++)
    {
        src = fkConditionTables[i];
        col = fkConditionCols[i];
        conditions << conditionTpl.arg(firstSrc, firstCol, src, col);
    }

    QString conditionsStr;
    if (!conditions.isEmpty()) {
        conditionsStr = conditionPrefixTpl.arg(conditions.join(", "));
    }

    // Current value column (will be 1 for row which matches current cell value)
    QVariant value = item->getValue();
    QString currValueColName = generateUniqueName("curr", usedNames);
    QString currValueExpr = value.isNull() ?
                currNullValueTpl.arg(firstSrcCol, currValueColName) :
                currValueTpl.arg(firstSrcCol, wrapValueIfNeeded(value), currValueColName);

    return sql.arg(
                selectedCols.join(", "),
                fkConditionTables.join(", "),
                conditionsStr,
                currValueExpr
                );
}

qlonglong SqlQueryItemDelegate::getRowCountForFkEditor(Db* db, const QString& query, bool* isError) const
{
    static_qstring(tpl, "SELECT count(*) FROM (%1)");

    QString sql = tpl.arg(query);
    SqlQueryPtr result = db->exec(sql);
    if (isError)
        *isError = result->isError();

    return result->getSingleCell().toLongLong();
}

int SqlQueryItemDelegate::getFkViewHeaderWidth(SqlQueryView* fkView, bool includeScrollBar) const
{
    int wd = fkView->horizontalHeader()->length();
    if (includeScrollBar && fkView->verticalScrollBar()->isVisible())
        wd += fkView->verticalScrollBar()->width();

    return wd;
}

QWidget* SqlQueryItemDelegate::getFkEditor(SqlQueryItem* item, QWidget* parent, const SqlQueryModel* model) const
{
    QString sql = getSqlForFkEditor(item);

    Db* db = model->getDb();
    bool countingError = false;
    qlonglong rowCount = getRowCountForFkEditor(db, sql, &countingError);
    if (rowCount > MAX_ROWS_FOR_FK)
    {
        notifyWarn(tr("Foreign key for column %2 has more than %1 possible values. It's too much to display in drop down list. You need to edit value manually.")
                   .arg(MAX_ROWS_FOR_FK).arg(item->getColumn()->column));

        return getEditor(item->getValue().userType(), parent);
    }

    if (rowCount == 0 && countingError && model->isStructureOutOfDate())
    {
        notifyWarn(tr("Cannot edit this cell. Details: %1").arg(tr("Structure of this table has changed since last data was loaded. Reload the data to proceed.")));
        return nullptr;
    }

    QComboBox *cb = new QComboBox(parent);
    cb->setEditable(true);

    // Prepare combo dropdown view.
    SqlQueryView* comboView = new SqlQueryView();
    comboView->setSimpleBrowserMode(true);
    comboView->setMaximumWidth(QGuiApplication::primaryScreen()->size().width());

    fkViewParentItemSize = model->getView()->horizontalHeader()->sectionSize(item->index().column());
    connect(comboView->horizontalHeader(), &QHeaderView::sectionResized, [this, comboView, model](int, int, int)
    {
        if (!model->isAllDataLoaded())
            return;

        updateComboViewGeometry(comboView, false);
    });

    SqlQueryModel* comboModel = new SqlQueryModel(comboView);
    comboModel->setView(comboView);

    // Mapping of model to cb, so we can update combo when data arrives.
    modelToFkCombo[comboModel] = cb;
    connect(cb, &QComboBox::destroyed, [this, comboModel](QObject*)
    {
        modelToFkCombo.remove(comboModel);
        modelToFkInitialValue.remove(comboModel);
    });

    // When execution is done, update combo.
    connect(comboModel, SIGNAL(aboutToLoadResults()), this, SLOT(fkDataAboutToLoad()));
    connect(comboModel, SIGNAL(executionSuccessful()), this, SLOT(fkDataReady()));
    connect(comboModel, SIGNAL(executionFailed(QString)), this, SLOT(fkDataFailed(QString)));

    // Setup combo, model, etc.
    cb->setModel(comboModel);
    cb->setView(comboView);
    cb->setModelColumn(1);
    cb->setCurrentText(item->getValue().toString());
    cb->view()->viewport()->installEventFilter(new FkComboShowFilter(this, comboView, cb));
    cb->view()->verticalScrollBar()->installEventFilter(new FkComboShowFilter(this, comboView, cb));
    if (!item->getValue().isNull())
        cb->lineEdit()->selectAll();

    comboModel->setHardRowLimit(MAX_ROWS_FOR_FK);
    comboModel->setCellDataLengthLimit(FK_CELL_LENGTH_LIMIT);
    comboModel->setDb(db);
    comboModel->setQuery(sql);
    comboModel->setAsyncMode(true);
    comboModel->executeQuery();

    comboView->verticalHeader()->setVisible(false);
    comboView->horizontalHeader()->setVisible(true);
    comboView->setSelectionMode(QAbstractItemView::SingleSelection);
    comboView->setSelectionBehavior(QAbstractItemView::SelectRows);

    return cb;
}

void SqlQueryItemDelegate::fkDataAboutToLoad()
{
    SqlQueryModel* model = dynamic_cast<SqlQueryModel*>(sender());
    if (!modelToFkCombo.contains(model))
        return;

    QComboBox* cb = modelToFkCombo[model];
    modelToFkInitialValue[model] = cb->currentText();
}

void SqlQueryItemDelegate::fkDataReady()
{
    SqlQueryModel* model = dynamic_cast<SqlQueryModel*>(sender());
    if (!modelToFkCombo.contains(model))
    {
        qDebug() << "Deceived fkDataReady() call when there is no longer a combo linked to the model.";
        return;
    }

    SqlQueryView* queryView = model->getView();

    queryView->horizontalHeader()->setSectionHidden(0, true);
    queryView->resizeColumnsToContents();
    queryView->resizeRowsToContents();

    updateComboViewGeometry(queryView, true);

    // Set selected combo value to initial value from the cell
    QComboBox* cb = modelToFkCombo[model];
    QString valueFromQueryModel = modelToFkInitialValue[model].toString();

    if (model->rowCount() > 0)
    {
        QModelIndex startIdx = model->index(0, cb->modelColumn());
        QModelIndex endIdx = model->index(model->rowCount() - 1, cb->modelColumn());
        QModelIndexList idxList = model->findIndexes(startIdx, endIdx, SqlQueryItem::DataRole::VALUE, valueFromQueryModel, 1, true);

        if (idxList.size() > 0)
        {
            cb->setCurrentIndex(idxList.first().row());
        }
        else
        {
            cb->setCurrentIndex(-1);
            cb->setEditText(valueFromQueryModel);
        }
    }
    else
        cb->setEditText(valueFromQueryModel);
}

void SqlQueryItemDelegate::fkDataFailed(const QString &errorText)
{
    notifyWarn(tr("Cannot edit this cell. Details: %1").arg(errorText));
}

void SqlQueryItemDelegate::updateComboViewGeometry(SqlQueryView* comboView, bool initial) const
{
    int wd = getFkViewHeaderWidth(comboView, true);
    comboView->setMinimumWidth(qMin(qMax(fkViewParentItemSize, wd), comboView->maximumWidth()));

    if (initial && wd < comboView->minimumWidth())
    {
        // First time, upon showing up
        int currentSize = comboView->horizontalHeader()->sectionSize(1);
        int gap = comboView->minimumWidth() - wd;
        comboView->horizontalHeader()->resizeSection(1, currentSize + gap);
    }

    QWidget* container = comboView->parentWidget();
    if (container->width() > comboView->minimumWidth())
    {
        container->setMaximumWidth(comboView->minimumWidth());
        container->resize(comboView->minimumWidth(), container->height());
    }
}

SqlQueryItemDelegate::FkComboShowFilter::FkComboShowFilter(const SqlQueryItemDelegate* delegate, SqlQueryView* comboView, QObject* parent)
    : QObject(parent), delegate(delegate), comboView(comboView)
{
}

bool SqlQueryItemDelegate::FkComboShowFilter::eventFilter(QObject* obj, QEvent* event)
{
    UNUSED(obj);
    if (event->type() == QEvent::Show)
        delegate->updateComboViewGeometry(comboView, true);

    return false;
}
